#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "sensor_gateway_config.h"

static const char *TAG = "sensor_gateway";

#define WIFI_CONNECTED_BIT BIT0
#define MQTT_TOPIC_MAX_LEN 128
#define MQTT_PAYLOAD_MAX_LEN 384
#define COAP_PACKET_MAX_LEN 512

static EventGroupHandle_t s_wifi_event_group;
static uint16_t s_coap_mid;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = LAB2_WIFI_SSID,
            .password = LAB2_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

static bool copy_event_field(char *dst, size_t dst_size, const char *src, int len)
{
    if (len < 0 || (size_t)len >= dst_size) {
        return false;
    }
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
    return true;
}

static bool mqtt_topic_part(const char *topic, int index, char *out, size_t out_size)
{
    int current = 0;
    const char *start = topic;

    for (const char *p = topic; ; ++p) {
        if (*p == '/' || *p == '\0') {
            if (current == index) {
                size_t len = (size_t)(p - start);
                if (len == 0 || len >= out_size) {
                    return false;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return true;
            }
            if (*p == '\0') {
                return false;
            }
            current++;
            start = p + 1;
        }
    }
}

static bool coap_add_option(uint8_t *packet, size_t *offset, size_t packet_len,
                            uint16_t *last_option, uint16_t option_number,
                            const uint8_t *value, size_t value_len)
{
    uint16_t delta = option_number - *last_option;
    if (delta > 12 || value_len > 12 || *offset + 1 + value_len > packet_len) {
        return false;
    }

    packet[(*offset)++] = (uint8_t)((delta << 4) | value_len);
    memcpy(&packet[*offset], value, value_len);
    *offset += value_len;
    *last_option = option_number;
    return true;
}

static int coap_send_request(const char *method, const char *path,
                             const char *payload, size_t payload_len)
{
    uint8_t packet[COAP_PACKET_MAX_LEN];
    size_t offset = 0;
    uint16_t last_option = 0;
    uint8_t code = strcmp(method, "PUT") == 0 ? 0x03 : 0x02;

    if (payload_len > MQTT_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "Payload too large for CoAP bridge: %u B", (unsigned)payload_len);
        return -1;
    }

    packet[offset++] = 0x40;
    packet[offset++] = code;
    packet[offset++] = (uint8_t)(s_coap_mid >> 8);
    packet[offset++] = (uint8_t)(s_coap_mid & 0xff);
    s_coap_mid++;

    const char *segment = path;
    while (*segment == '/') {
        segment++;
    }

    while (*segment) {
        const char *slash = strchr(segment, '/');
        size_t len = slash ? (size_t)(slash - segment) : strlen(segment);
        if (!coap_add_option(packet, &offset, sizeof(packet), &last_option,
                             11, (const uint8_t *)segment, len)) {
            ESP_LOGE(TAG, "Could not encode CoAP Uri-Path option");
            return -1;
        }
        if (!slash) {
            break;
        }
        segment = slash + 1;
    }

    const uint8_t json_format = 50;
    if (!coap_add_option(packet, &offset, sizeof(packet), &last_option,
                         12, &json_format, sizeof(json_format))) {
        ESP_LOGE(TAG, "Could not encode CoAP Content-Format option");
        return -1;
    }

    if (payload_len > 0) {
        if (offset + 1 + payload_len > sizeof(packet)) {
            ESP_LOGE(TAG, "CoAP packet buffer is too small");
            return -1;
        }
        packet[offset++] = 0xff;
        memcpy(&packet[offset], payload, payload_len);
        offset += payload_len;
    }

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(LAB2_BORDER_ROUTER_HOST, LAB2_BORDER_ROUTER_COAP_PORT,
                          &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "Cannot resolve border router '%s': %d",
                 LAB2_BORDER_ROUTER_HOST, err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    int sent = sendto(sock, packet, offset, 0, res->ai_addr, res->ai_addrlen);
    close(sock);
    freeaddrinfo(res);

    if (sent < 0) {
        ESP_LOGE(TAG, "CoAP send failed: errno=%d", errno);
        return -1;
    }

    ESP_LOGI(TAG, "CoAP %s %s -> %d B", method, path, sent);
    return 0;
}

static void forward_telemetry(const char *topic, const char *payload, size_t payload_len)
{
    char node[32];
    char path[96];

    if (!mqtt_topic_part(topic, 2, node, sizeof(node))) {
        ESP_LOGW(TAG, "Telemetry topic without node id: %s", topic);
        return;
    }

    snprintf(path, sizeof(path), "/sensors/%s/telemetry", node);
    coap_send_request("POST", path, payload, payload_len);
}

static void forward_actuator_command(const char *topic, const char *payload, size_t payload_len)
{
    char node[32];
    char actuator[32];
    char path[96];

    if (!mqtt_topic_part(topic, 2, node, sizeof(node)) ||
        !mqtt_topic_part(topic, 3, actuator, sizeof(actuator))) {
        ESP_LOGW(TAG, "Actuator topic without node/actuator ids: %s", topic);
        return;
    }

    snprintf(path, sizeof(path), "/actuators/%s/%s", node, actuator);
    coap_send_request("PUT", path, payload, payload_len);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected: %s", LAB2_MQTT_BROKER_URI);
        esp_mqtt_client_subscribe(client, LAB2_MQTT_TELEMETRY_TOPIC, 1);
        esp_mqtt_client_subscribe(client, LAB2_MQTT_ACTUATOR_COMMAND_TOPIC, 1);
        break;

    case MQTT_EVENT_DATA: {
        char topic[MQTT_TOPIC_MAX_LEN];
        char payload[MQTT_PAYLOAD_MAX_LEN + 1];

        if (!copy_event_field(topic, sizeof(topic), event->topic, event->topic_len) ||
            !copy_event_field(payload, sizeof(payload), event->data, event->data_len)) {
            ESP_LOGW(TAG, "MQTT message too large; topic=%d payload=%d",
                     event->topic_len, event->data_len);
            break;
        }

        ESP_LOGI(TAG, "MQTT rx %s: %s", topic, payload);
        if (strncmp(topic, "lab2/sensors/", strlen("lab2/sensors/")) == 0) {
            forward_telemetry(topic, payload, (size_t)event->data_len);
        } else if (strncmp(topic, "lab2/actuators/", strlen("lab2/actuators/")) == 0) {
            forward_actuator_command(topic, payload, (size_t)event->data_len);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = LAB2_MQTT_BROKER_URI,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

void start_sensor_gateway(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_coap_mid = (uint16_t)(esp_random() & 0xffff);
    wifi_start();
    mqtt_start();
}
