#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "coap3/coap.h"
#include "sensor_gateway_config.h"

static const char *TAG = "valve_demo";
#define COAP_PORT       5683
#define VALVE_GPIO      LAB2_PUMP_CONTROL_GPIO

static uint8_t g_valve_state = 0;
static coap_resource_t *g_valve_resource = NULL;

// Preferred payload is exactly 4 bytes: A1 61 76 0X, which is CBOR {"v": 0|1}.
// The OpenThread CLI used in this project sends payload arguments as text, so
// accept "a1617600" / "a1617601" as a lab CLI convenience as well.
static bool decode_valve_cbor(const uint8_t *buf, size_t len, uint8_t *out_v)
{
    if (len == 4) {
        if (buf[0] != 0xA1) {
            return false;
        }
        if (buf[1] != 0x61 || buf[2] != 0x76) {
            return false;
        }
        if (buf[3] != 0x00 && buf[3] != 0x01) {
            return false;
        }
        *out_v = buf[3];
        return true;
    }

    if (len == 8) {
        bool prefix_ok = (buf[0] == 'a' || buf[0] == 'A') &&
                         buf[1] == '1' && buf[2] == '6' && buf[3] == '1' &&
                         buf[4] == '7' && buf[5] == '6' && buf[6] == '0';
        if (prefix_ok && (buf[7] == '0' || buf[7] == '1')) {
            *out_v = (uint8_t)(buf[7] - '0');
            return true;
        }
    }

    return false;
}

static size_t encode_valve_cbor(uint8_t v, uint8_t out[4])
{
    out[0] = 0xA1;
    out[1] = 0x61;
    out[2] = 0x76;
    out[3] = v ? 0x01 : 0x00;
    return 4;
}

static void apply_valve_state(uint8_t v)
{
    gpio_set_level(VALVE_GPIO, v);
    g_valve_state = v;
    ESP_LOGI(TAG, "valve -> %s (GPIO%d = %d)", v ? "OPEN" : "CLOSED", VALVE_GPIO, v);
}

// PUT is idempotent: applying the same value twice leaves the same valve state.
static void hnd_valve_put(coap_resource_t *resource,
                          coap_session_t  *session,
                          const coap_pdu_t *request,
                          const coap_string_t *query,
                          coap_pdu_t      *response)
{
    (void)resource;
    (void)session;
    (void)query;

    size_t len = 0;
    const uint8_t *data = NULL;
    coap_get_data(request, &len, &data);

    uint8_t v;
    if (!decode_valve_cbor(data, len, &v)) {
        ESP_LOGW(TAG, "PUT /act/valve: malformed CBOR (%u B)", (unsigned)len);
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_BAD_REQUEST);
        return;
    }

    apply_valve_state(v);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
}

static void hnd_valve_get(coap_resource_t *resource,
                          coap_session_t  *session,
                          const coap_pdu_t *request,
                          const coap_string_t *query,
                          coap_pdu_t      *response)
{
    (void)resource;
    (void)session;
    (void)request;
    (void)query;

    uint8_t buf[4];
    size_t len = encode_valve_cbor(g_valve_state, buf);

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);

    unsigned char encoded[4];
    coap_add_option(response, COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_safe(encoded, sizeof(encoded),
                                          COAP_MEDIATYPE_APPLICATION_CBOR),
                    encoded);
    coap_add_data(response, len, buf);

    ESP_LOGI(TAG, "GET /act/valve -> %u", g_valve_state);
}

static void valve_server_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << VALVE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    apply_valve_state(0);

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin6.sin6_family = AF_INET6;
    addr.addr.sin6.sin6_port   = htons(COAP_PORT);
    addr.addr.sin6.sin6_addr   = in6addr_any;

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

    g_valve_resource = coap_resource_init(coap_make_str_const("act/valve"), 0);
    coap_register_handler(g_valve_resource, COAP_REQUEST_PUT, hnd_valve_put);
    coap_register_handler(g_valve_resource, COAP_REQUEST_GET, hnd_valve_get);
    coap_add_resource(ctx, g_valve_resource);

    ESP_LOGI(TAG, "CoAP server listening on UDP/%d, resource /act/valve", COAP_PORT);
    while (1) {
        coap_io_process(ctx, 1000);
    }
}

void start_valve_server(void)
{
    xTaskCreate(valve_server_task, "valve_server", 6144, NULL, 5, NULL);
}
