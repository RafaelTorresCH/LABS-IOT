#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include <esp_http_server.h>
#include "cJSON.h"
#include "coap3/coap.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

// --- CONFIGURACIÓN WIFI ---
#define WIFI_SSID "WIFIPI"
#define WIFI_PASS "123456789"

static const char *TAG = "SCADA";

// --- VARIABLES GLOBALES ---
char currentMode[10] = "manual";
int valveState = 0;
int realHumA = 0, realHumS = 0, realLevel = 50; 

// Credenciales (Se cargan desde NVS)
char tg_token[64] = "";
char tg_chat[64] = "";
char db_url[128] = "";
char db_org[64] = "";
char db_bucket[64] = "";
char db_token[128] = "";

httpd_handle_t server = NULL;

static void broadcast_ws_msg();

// --- 1. MEMORIA NVS (Guardar y Cargar) ---
void load_nvs_config() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t len;
        len = sizeof(tg_token); nvs_get_str(my_handle, "tg_token", tg_token, &len);
        len = sizeof(tg_chat);  nvs_get_str(my_handle, "tg_chat", tg_chat, &len);
        len = sizeof(db_url);   nvs_get_str(my_handle, "db_url", db_url, &len);
        len = sizeof(db_org);   nvs_get_str(my_handle, "db_org", db_org, &len);
        len = sizeof(db_bucket);nvs_get_str(my_handle, "db_bucket", db_bucket, &len);
        len = sizeof(db_token); nvs_get_str(my_handle, "db_token", db_token, &len);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "NVS Cargado. Telegram: %s | InfluxDB: %s", 
                 strlen(tg_token) > 0 ? "OK" : "Vacio", 
                 strlen(db_url) > 0 ? "OK" : "Vacio");
    }
}

// --- 2. CLIENTES HTTP (Telegram e InfluxDB) ---
void send_telegram_msg(const char* msg) {
    if (strlen(tg_token) == 0 || strlen(tg_chat) == 0) return;

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s", tg_token, tg_chat, msg);

    esp_http_client_config_t config = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach, 
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return; 
    
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

void send_to_influxdb(int hA, int hS, int lvl) {
    if (strlen(db_url) == 0 || strlen(db_token) == 0) return; 

    // ---> CAMBIA ESTE 256 POR 512 <---
    char write_url[512]; 
    
    // Formato de la URL para escribir en InfluxDB v2
    snprintf(write_url, sizeof(write_url), "%s/api/v2/write?org=%s&bucket=%s&precision=s", db_url, db_org, db_bucket);

    char payload[128];
    // InfluxDB Line Protocol
    snprintf(payload, sizeof(payload), "telemetria hum_a=%d,hum_s=%d,level=%d", hA, hS, lvl);

    esp_http_client_config_t config = {
        .url = write_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, // Por si usas InfluxDB Cloud (HTTPS)
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return;

    // Cabeceras obligatorias para InfluxDB
    char auth_header[200];
    snprintf(auth_header, sizeof(auth_header), "Token %s", db_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Datos en InfluxDB -> Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Fallo enviando a InfluxDB: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

// --- 3. CONFIGURACIÓN DE RED (WiFi, SPIFFS, CoAP) ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Asignada: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS }, };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

void init_spiffs() {
    esp_vfs_spiffs_conf_t conf = { .base_path = "/data", .max_files = 5, .format_if_mount_failed = true };
    esp_vfs_spiffs_register(&conf);
}

static void hnd_post_sensores(coap_resource_t *resource, coap_session_t *session, const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response) {
    size_t size; const uint8_t *data;
    if (coap_get_data(request, &size, &data)) {
        char payload[128] = {0};
        strncpy(payload, (char *)data, size < sizeof(payload) ? size : sizeof(payload) - 1);
        cJSON *json = cJSON_Parse(payload);
        if (json != NULL) {
            cJSON *hA = cJSON_GetObjectItem(json, "hum_a");
            cJSON *hS = cJSON_GetObjectItem(json, "hum_s");
            cJSON *lvl = cJSON_GetObjectItem(json, "level");
            if (hA) realHumA = hA->valueint;
            if (hS) realHumS = hS->valueint;
            if (lvl) realLevel = lvl->valueint;
            cJSON_Delete(json);
            broadcast_ws_msg();
        }
    }
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
}

void coap_server_task(void *p) {
    coap_context_t *ctx = coap_new_context(NULL);
    coap_address_t serv_addr;
    coap_address_init(&serv_addr);
    serv_addr.addr.sin.sin_family = AF_INET;
    serv_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    serv_addr.addr.sin.sin_port = htons(COAP_DEFAULT_PORT);
    coap_new_endpoint(ctx, &serv_addr, COAP_PROTO_UDP);
    coap_str_const_t *r_uri = coap_make_str_const("sensores");
    coap_resource_t *resource = coap_resource_init(r_uri, 0);
    coap_register_handler(resource, COAP_REQUEST_POST, hnd_post_sensores);
    coap_add_resource(ctx, resource);
    while (1) coap_io_process(ctx, 1000); 
}

// --- 4. SERVIDOR HTTP Y WEBSOCKETS ---
static esp_err_t index_html_handler(httpd_req_t *req) {
    FILE* f = fopen("/data/index.html", "r");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    char buffer[1024]; size_t read_bytes;
    httpd_resp_set_type(req, "text/html");
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) httpd_resp_send_chunk(req, buffer, read_bytes);
    httpd_resp_send_chunk(req, NULL, 0); 
    fclose(f); return ESP_OK;
}

struct async_resp_arg { httpd_handle_t hd; int fd; };

static void ws_async_send(void *arg) {
    char buffer[256]; // Subí el tamaño del buffer a 256 para que quepa todo sin explotar
    
    // Aquí le inyectamos a la web la confirmación de si hay datos guardados
    snprintf(buffer, sizeof(buffer), 
             "{\"hum_a\":%d,\"hum_s\":%d,\"level\":%d,\"mode\":\"%s\",\"valve\":%d,\"has_tg\":%s,\"has_db\":%s}", 
             realHumA, realHumS, realLevel, currentMode, valveState,
             strlen(tg_token) > 0 ? "true" : "false",
             strlen(db_url) > 0 ? "true" : "false");
             
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)buffer;
    ws_pkt.len = strlen(buffer);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    struct async_resp_arg *resp_arg = arg;
    httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
    free(resp_arg);
}
static void broadcast_ws_msg() {
    if (!server) return;
    size_t clients = 8; int client_fds[8];
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; ++i) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                struct async_resp_arg *arg = malloc(sizeof(struct async_resp_arg));
                arg->hd = server; arg->fd = client_fds[i];
                httpd_queue_work(arg->hd, ws_async_send, arg);
            }
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK; 
    httpd_ws_frame_t ws_pkt;
    uint8_t buf[512] = { 0 }; 
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = buf;
    
    if (httpd_ws_recv_frame(req, &ws_pkt, 512) != ESP_OK) return ESP_FAIL;

    cJSON *json = cJSON_Parse((char*)ws_pkt.payload);
    if (json != NULL) {
        cJSON *config_update = cJSON_GetObjectItem(json, "config_update");
        if (config_update && cJSON_IsTrue(config_update)) {
            // Extraer InfluxDB y Telegram
            cJSON *t_token = cJSON_GetObjectItem(json, "tg_token");
            cJSON *t_chat  = cJSON_GetObjectItem(json, "tg_chat");
            cJSON *d_url   = cJSON_GetObjectItem(json, "db_url");
            cJSON *d_org   = cJSON_GetObjectItem(json, "db_org");
            cJSON *d_buck  = cJSON_GetObjectItem(json, "db_bucket");
            cJSON *d_token = cJSON_GetObjectItem(json, "db_token");
            
            nvs_handle_t h;
            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                if (t_token && cJSON_IsString(t_token) && strlen(t_token->valuestring) > 0) {
                    strncpy(tg_token, t_token->valuestring, sizeof(tg_token)-1); nvs_set_str(h, "tg_token", tg_token);
                }
                if (t_chat && cJSON_IsString(t_chat) && strlen(t_chat->valuestring) > 0) {
                    strncpy(tg_chat, t_chat->valuestring, sizeof(tg_chat)-1); nvs_set_str(h, "tg_chat", tg_chat);
                }
                if (d_url && cJSON_IsString(d_url) && strlen(d_url->valuestring) > 0) {
                    strncpy(db_url, d_url->valuestring, sizeof(db_url)-1); nvs_set_str(h, "db_url", db_url);
                }
                if (d_org && cJSON_IsString(d_org) && strlen(d_org->valuestring) > 0) {
                    strncpy(db_org, d_org->valuestring, sizeof(db_org)-1); nvs_set_str(h, "db_org", db_org);
                }
                if (d_buck && cJSON_IsString(d_buck) && strlen(d_buck->valuestring) > 0) {
                    strncpy(db_bucket, d_buck->valuestring, sizeof(db_bucket)-1); nvs_set_str(h, "db_bucket", db_bucket);
                }
                if (d_token && cJSON_IsString(d_token) && strlen(d_token->valuestring) > 0) {
                    strncpy(db_token, d_token->valuestring, sizeof(db_token)-1); nvs_set_str(h, "db_token", db_token);
                }
                nvs_commit(h); nvs_close(h);
                ESP_LOGI(TAG, "Configuraciones guardadas en NVS.");
                send_telegram_msg("SCADA%20actualizado.%20Telegram%20e%20InfluxDB%20listos.");
            }
        }

        cJSON *mode = cJSON_GetObjectItem(json, "mode");
        if (mode && cJSON_IsString(mode)) strncpy(currentMode, mode->valuestring, sizeof(currentMode)-1);
        
        cJSON *valve = cJSON_GetObjectItem(json, "valve");
        if (valve && cJSON_IsNumber(valve) && strcmp(currentMode, "manual") == 0) valveState = valve->valueint;
        
        cJSON_Delete(json);
    }
    broadcast_ws_msg();
    return ESP_OK;
}

void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240; 
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = index_html_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_get);
        httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true };
        httpd_register_uri_handler(server, &ws_uri);
    }
}

// --- 5. TAREA FREERTOS ---
void controlTask(void *pvParameters) {
    bool alert_high = false;
    bool alert_low = false;
    int influx_timer = 0;

    for (;;) {
        // Control Válvula
        if (strcmp(currentMode, "auto") == 0) {
            if (realLevel >= 90 && valveState == 1) { valveState = 0; broadcast_ws_msg(); }
            else if (realLevel <= 10 && valveState == 0) { valveState = 1; broadcast_ws_msg(); }
        }

        // Alarmas Telegram
        if (realLevel >= 90 && !alert_high) {
            send_telegram_msg("ALERTA:%20Nivel%20de%20tanque%20critico");
            alert_high = true; alert_low = false;
        } else if (realLevel <= 10 && !alert_low) {
            send_telegram_msg("ALERTA:%20Nivel%20de%20tanque%20bajo");
            alert_low = true; alert_high = false;
        } else if (realLevel > 10 && realLevel < 90) {
            alert_high = false; alert_low = false;
        }

        // Reloj InfluxDB (Envía datos cada 5 segundos)
        influx_timer++;
        if (influx_timer >= 5) {
            send_to_influxdb(realHumA, realHumS, realLevel);
            influx_timer = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- MAIN ---
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    
    load_nvs_config();
    
    init_spiffs();
    wifi_init_sta();
    start_webserver();
    
    xTaskCreatePinnedToCore(controlTask, "Control", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(coap_server_task, "CoAP", 8192, NULL, 1, NULL, 1);
}