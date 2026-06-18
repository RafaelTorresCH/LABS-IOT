#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "coap3/coap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensor_gateway_config.h"

static const char *TAG = "coap_demo";
#define COAP_PORT 5683

typedef struct {
    bool valid;
    float temperature_c;
    float air_humidity_pct;
    int soil_raw;
    float soil_pct;
    int water_raw;
    float water_pct;
} sensor_reading_t;

extern sensor_reading_t read_sensors(void);

static sensor_reading_t g_current_sample = {0};
static sensor_reading_t g_last_notified_sample = {0};
static int64_t g_last_notify_us = 0;
static coap_resource_t *g_env_temp_resource = NULL;

static size_t cbor_put_type(uint8_t *buf, size_t idx, uint8_t major, uint64_t value)
{
    if (value < 24) {
        buf[idx++] = (uint8_t)((major << 5) | value);
        return idx;
    }
    if (value <= 0xFF) {
        buf[idx++] = (uint8_t)((major << 5) | 24);
        buf[idx++] = (uint8_t)value;
        return idx;
    }
    if (value <= 0xFFFF) {
        buf[idx++] = (uint8_t)((major << 5) | 25);
        buf[idx++] = (uint8_t)(value >> 8);
        buf[idx++] = (uint8_t)(value & 0xFF);
        return idx;
    }

    buf[idx++] = (uint8_t)((major << 5) | 26);
    buf[idx++] = (uint8_t)(value >> 24);
    buf[idx++] = (uint8_t)(value >> 16);
    buf[idx++] = (uint8_t)(value >> 8);
    buf[idx++] = (uint8_t)(value & 0xFF);
    return idx;
}

static size_t cbor_put_uint(uint8_t *buf, size_t idx, uint64_t value)
{
    return cbor_put_type(buf, idx, 0, value);
}

static size_t cbor_put_nint(uint8_t *buf, size_t idx, int64_t value)
{
    return cbor_put_type(buf, idx, 1, (uint64_t)(-1 - value));
}

static size_t cbor_put_tstr(uint8_t *buf, size_t idx, const char *s)
{
    size_t len = strlen(s);
    idx = cbor_put_type(buf, idx, 3, len);
    memcpy(&buf[idx], s, len);
    return idx + len;
}

static size_t encode_telemetry_cbor(const sensor_reading_t *sample, uint8_t *buf, size_t max_len)
{
    if (max_len < 64) {
        return 0;
    }

    const int temp_x10 = sample->valid ? (int)lroundf(sample->temperature_c * 10.0f) : -9990;
    const int hum_x10 = sample->valid ? (int)lroundf(sample->air_humidity_pct * 10.0f) : -1;
    const int soil_x10 = (int)lroundf(sample->soil_pct * 10.0f);

    size_t idx = 0;
    buf[idx++] = 0xA4;
    idx = cbor_put_tstr(buf, idx, "t_x10");
    idx = (temp_x10 >= 0) ? cbor_put_uint(buf, idx, (uint64_t)temp_x10) : cbor_put_nint(buf, idx, (int64_t)temp_x10);
    idx = cbor_put_tstr(buf, idx, "h_x10");
    idx = (hum_x10 >= 0) ? cbor_put_uint(buf, idx, (uint64_t)hum_x10) : cbor_put_nint(buf, idx, (int64_t)hum_x10);
    idx = cbor_put_tstr(buf, idx, "soil_x10");
    idx = (soil_x10 >= 0) ? cbor_put_uint(buf, idx, (uint64_t)soil_x10) : cbor_put_nint(buf, idx, (int64_t)soil_x10);
    idx = cbor_put_tstr(buf, idx, "node");
    idx = cbor_put_tstr(buf, idx, LAB2_SENSOR_NODE_ID);
    return idx;
}

static void hnd_env_temp_get(coap_resource_t *resource,
                             coap_session_t *session,
                             const coap_pdu_t *request,
                             const coap_string_t *query,
                             coap_pdu_t *response)
{
    (void)resource;
    (void)session;
    (void)request;
    (void)query;

    g_current_sample = read_sensors();

    uint8_t buf[64];
    size_t len = encode_telemetry_cbor(&g_current_sample, buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGW(TAG, "Could not encode telemetry payload");
        return;
    }

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);

    unsigned char encoded[4];
    coap_add_option(response, COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_safe(encoded, sizeof(encoded),
                                          COAP_MEDIATYPE_APPLICATION_CBOR),
                    encoded);
    coap_add_option(response, COAP_OPTION_MAXAGE,
                    coap_encode_var_safe(encoded, sizeof(encoded),
                                          LAB2_SENSOR_SAMPLE_PERIOD_MS / 1000),
                    encoded);
    coap_add_data(response, len, buf);

    ESP_LOGI(TAG, "GET /env/temp -> temp=%.1f C humidity=%.1f %% soil=%.1f %% raw=%d",
             g_current_sample.temperature_c,
             g_current_sample.air_humidity_pct,
             g_current_sample.soil_pct,
             g_current_sample.soil_raw);
}

static bool sample_changed_enough(const sensor_reading_t *a, const sensor_reading_t *b)
{
    if (a->valid != b->valid) {
        return true;
    }
    if (!a->valid) {
        return false;
    }
    return fabsf(a->temperature_c - b->temperature_c) > 0.5f ||
           fabsf(a->air_humidity_pct - b->air_humidity_pct) > 1.0f ||
           fabsf(a->soil_pct - b->soil_pct) > 1.0f ||
           a->soil_raw != b->soil_raw;
}

static void update_env_temp_observers(void)
{
    sensor_reading_t fresh = read_sensors();
    bool first_sample = !g_last_notify_us;

    if (first_sample || sample_changed_enough(&fresh, &g_last_notified_sample)) {
        g_current_sample = fresh;
        if (g_env_temp_resource && coap_resource_notify_observers(g_env_temp_resource, NULL)) {
            g_last_notified_sample = fresh;
            g_last_notify_us = esp_timer_get_time();
            ESP_LOGI(TAG, "notify: temp=%.1f C humidity=%.1f %% soil=%.1f %% raw=%d",
                     fresh.temperature_c,
                     fresh.air_humidity_pct,
                     fresh.soil_pct,
                     fresh.soil_raw);
        }
    }
}

static void coap_server_task(void *pvParameters)
{
    (void)pvParameters;

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin6.sin6_family = AF_INET6;
    addr.addr.sin6.sin6_port = htons(COAP_PORT);
    addr.addr.sin6.sin6_addr = in6addr_any;

    coap_set_log_level(COAP_LOG_WARN);
    coap_context_t *ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "coap_new_context failed");
        vTaskDelete(NULL);
        return;
    }
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP);

    if (!coap_new_endpoint(ctx, &addr, COAP_PROTO_UDP)) {
        ESP_LOGE(TAG, "coap_new_endpoint failed");
        coap_free_context(ctx);
        vTaskDelete(NULL);
        return;
    }

    g_env_temp_resource = coap_resource_init(coap_make_str_const("env/temp"), 0);
    coap_register_handler(g_env_temp_resource, COAP_REQUEST_GET, hnd_env_temp_get);
    coap_resource_set_get_observable(g_env_temp_resource, 1);
    coap_add_resource(ctx, g_env_temp_resource);

    ESP_LOGI(TAG, "CoAP server listening on UDP/%d, resource /env/temp", COAP_PORT);

    g_last_notify_us = esp_timer_get_time();
    while (1) {
        coap_io_process(ctx, 1000);
        update_env_temp_observers();
    }
}

void start_coap_server(void)
{
    xTaskCreate(coap_server_task, "coap_server", 6144, NULL, 5, NULL);
}
