#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "coap3/coap.h"

static const char *TAG = "nivel_coap";
#define COAP_PORT 5683

int water_level_percent(void);
int water_level_raw_state(void);
int water_pump_state(void);

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

static size_t cbor_put_tstr(uint8_t *buf, size_t idx, const char *s)
{
    size_t len = strlen(s);
    idx = cbor_put_type(buf, idx, 3, len);
    memcpy(&buf[idx], s, len);
    return idx + len;
}

static size_t encode_level_cbor(uint8_t *buf, size_t max_len)
{
    int level_pct = water_level_percent();
    int raw_state = water_level_raw_state();
    int pump_state = water_pump_state();
    size_t idx = 0;

    if (max_len < 64) {
        return 0;
    }

    buf[idx++] = 0xA4;
    idx = cbor_put_tstr(buf, idx, "level");
    idx = cbor_put_uint(buf, idx, (uint64_t)level_pct);
    idx = cbor_put_tstr(buf, idx, "raw");
    idx = cbor_put_uint(buf, idx, (uint64_t)raw_state);
    idx = cbor_put_tstr(buf, idx, "pump");
    idx = cbor_put_uint(buf, idx, (uint64_t)pump_state);
    idx = cbor_put_tstr(buf, idx, "node");
    idx = cbor_put_tstr(buf, idx, "nodo2");
    return idx;
}

static void hnd_level_get(coap_resource_t *resource,
                          coap_session_t *session,
                          const coap_pdu_t *request,
                          const coap_string_t *query,
                          coap_pdu_t *response)
{
    (void)resource;
    (void)session;
    (void)request;
    (void)query;

    uint8_t buf[64];
    size_t len = encode_level_cbor(buf, sizeof(buf));
    unsigned char encoded[4];

    if (len == 0) {
        ESP_LOGW(TAG, "Could not encode /nivel payload");
        return;
    }

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_option(response, COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_safe(encoded, sizeof(encoded),
                                         COAP_MEDIATYPE_APPLICATION_CBOR),
                    encoded);
    coap_add_data(response, len, buf);

    ESP_LOGI(TAG, "GET /nivel -> level=%d raw=%d pump=%d",
             water_level_percent(), water_level_raw_state(), water_pump_state());
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

    coap_resource_t *level_resource = coap_resource_init(coap_make_str_const("nivel"), 0);
    coap_register_handler(level_resource, COAP_REQUEST_GET, hnd_level_get);
    coap_add_resource(ctx, level_resource);

    ESP_LOGI(TAG, "CoAP server listening on UDP/%d, resource /nivel", COAP_PORT);

    while (1) {
        coap_io_process(ctx, 1000);
    }
}

void start_coap_server(void)
{
    xTaskCreate(coap_server_task, "level_server", 6144, NULL, 5, NULL);
}
