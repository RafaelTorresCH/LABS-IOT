#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "coap3/coap.h"

static const char *TAG = "coap_demo";

#define COAP_PORT 5683

#define ENV_TEMP_MAX_AGE_S    60u
#define ENV_TEMP_HEARTBEAT_S  45u

static float g_current_temp = 24.5f;
static float g_last_notified_temp = 24.5f;
static int64_t g_last_notify_us = 0;

static coap_resource_t *g_env_temp_resource = NULL;

/* ============================================================
 * FLOAT32 -> FLOAT16
 * ============================================================ */

static uint16_t float32_to_float16(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof(x));

    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp   = ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (x >> 13) & 0x3FF;

    if (exp <= 0) {
        return (uint16_t)sign;
    }

    if (exp >= 31) {
        return (uint16_t)(sign | 0x7C00);
    }

    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

/* ============================================================
 * CBOR ENCODER
 * Payload:
 * {
 *   "t": <float16>
 * }
 * ============================================================ */

static size_t encode_env_temp_cbor(float value, uint8_t out[6])
{
    uint16_t h = float32_to_float16(value);

    out[0] = 0xA1;
    out[1] = 0x61;
    out[2] = 0x74;
    out[3] = 0xF9;
    out[4] = (uint8_t)(h >> 8);
    out[5] = (uint8_t)(h & 0xFF);

    return 6;
}

/* ============================================================
 * GET HANDLER
 * URI: /env
 * ============================================================ */

static void hnd_env_get(coap_resource_t *resource,
                        coap_session_t *session,
                        const coap_pdu_t *request,
                        const coap_string_t *query,
                        coap_pdu_t *response)
{
    (void)resource;
    (void)session;
    (void)request;
    (void)query;

    uint8_t buf[6];

    size_t len = encode_env_temp_cbor(g_current_temp, buf);

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);

    unsigned char encoded[4];

    coap_add_option(response,
                    COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_safe(encoded,
                                          sizeof(encoded),
                                          COAP_MEDIATYPE_APPLICATION_CBOR),
                    encoded);

    coap_add_option(response,
                    COAP_OPTION_MAXAGE,
                    coap_encode_var_safe(encoded,
                                          sizeof(encoded),
                                          ENV_TEMP_MAX_AGE_S),
                    encoded);

    coap_add_data(response, len, buf);

    ESP_LOGI(TAG,
             "GET /env -> %.2f C (6-byte CBOR)",
             g_current_temp);
}

/* ============================================================
 * SENSOR MOCK + OBSERVE NOTIFICATIONS
 * ============================================================ */

static void temp_update_task(void *arg)
{
    coap_context_t *ctx = (coap_context_t *)arg;

    const float baseline = 24.5f;
    const float amp_c    = 1.0f;
    const float period_s = 60.0f;

    g_last_notify_us = esp_timer_get_time();

    while (1) {

        float t_s =
            (float)esp_timer_get_time() / 1e6f;

        float noise =
            ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 0.2f;

        g_current_temp =
            baseline +
            amp_c * sinf(2.0f * (float)M_PI * t_s / period_s) +
            noise;

        float diff =
            fabsf(g_current_temp - g_last_notified_temp);

        int64_t since_us =
            esp_timer_get_time() - g_last_notify_us;

        bool threshold =
            diff > 0.5f;

        bool heartbeat =
            since_us >
            (int64_t)ENV_TEMP_HEARTBEAT_S * 1000000;

        if (threshold || heartbeat) {

            ESP_LOGI(TAG,
                     "notify (%s): T=%.2f C, Δ=%.2f C, %lld s since last",
                     threshold ? "threshold" : "heartbeat",
                     g_current_temp,
                     diff,
                     since_us / 1000000);

            g_last_notified_temp = g_current_temp;
            g_last_notify_us     = esp_timer_get_time();

            if (g_env_temp_resource) {
                coap_resource_notify_observers(
                    g_env_temp_resource,
                    NULL);
            }
        }

        coap_io_process(ctx, 0);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ============================================================
 * COAP SERVER TASK
 * ============================================================ */

static void coap_server_task(void *pvParameters)
{
    (void)pvParameters;

    coap_address_t addr;

    coap_address_init(&addr);

    addr.addr.sin6.sin6_family = AF_INET6;
    addr.addr.sin6.sin6_port   = htons(COAP_PORT);
    addr.addr.sin6.sin6_addr   = in6addr_any;

    coap_set_log_level(COAP_LOG_WARN);

    coap_context_t *ctx =
        coap_new_context(NULL);

    if (!ctx) {
        ESP_LOGE(TAG, "coap_new_context failed");
        vTaskDelete(NULL);
        return;
    }

    coap_context_set_block_mode(
        ctx,
        COAP_BLOCK_USE_LIBCOAP);

    if (!coap_new_endpoint(ctx,
                           &addr,
                           COAP_PROTO_UDP)) {

        ESP_LOGE(TAG,
                 "coap_new_endpoint failed");

        coap_free_context(ctx);

        vTaskDelete(NULL);

        return;
    }

    /* IMPORTANT:
     * Use "env" instead of "env/temp"
     * because OpenThread CLI sends URI path
     * segments independently.
     */

    g_env_temp_resource =
        coap_resource_init(
            coap_make_str_const("env"),
            0);

    coap_register_handler(
        g_env_temp_resource,
        COAP_REQUEST_GET,
        hnd_env_get);

    coap_resource_set_get_observable(
        g_env_temp_resource,
        1);

    coap_add_resource(
        ctx,
        g_env_temp_resource);

    ESP_LOGI(TAG,
             "CoAP server listening on UDP/%d, resource /env",
             COAP_PORT);

    xTaskCreate(temp_update_task,
                "temp_update",
                4096,
                ctx,
                4,
                NULL);

    while (1) {
        coap_io_process(ctx, 1000);
    }
}

/* ============================================================
 * START SERVER
 * ============================================================ */

void start_coap_server(void)
{
    xTaskCreate(coap_server_task,
                "coap_server",
                6144,
                NULL,
                5,
                NULL);
}