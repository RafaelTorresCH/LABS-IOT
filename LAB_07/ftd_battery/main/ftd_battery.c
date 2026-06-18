#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "openthread/link.h"
#include "openthread/dataset.h"
#include "openthread/coap.h"
#include "openthread/logging.h"
#include "openthread/thread.h"

#include "esp_ot_config.h"

#if !SOC_IEEE802154_SUPPORTED
#error "OpenThread native radio requires IEEE 802.15.4 support"
#endif

#define TAG "soilsense_ftd"
#define HEALTH_URI "sys/health"
#define COAP_PORT 5683

static TaskHandle_t s_ot_task_handle;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static bool s_adc_cali_ready;
static adc_unit_t s_adc_unit_id;
static adc_channel_t s_adc_channel;
static otCoapResource s_health_resource;

static const char *thread_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_CHILD:
        return "child";
    case OT_DEVICE_ROLE_ROUTER:
        return "router";
    case OT_DEVICE_ROLE_LEADER:
        return "leader";
    case OT_DEVICE_ROLE_DETACHED:
        return "detached";
    default:
        return "disabled";
    }
}

static void force_child_role(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otError error;

    if (instance == NULL) {
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otThreadBecomeChild(instance);
    esp_openthread_lock_release();

    if (error != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "otThreadBecomeChild returned %d", error);
    }
}

static void log_thread_identity(void)
{
    otInstance *instance = esp_openthread_get_instance();
    const otExtAddress *extaddr = NULL;
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    uint16_t rloc16 = 0;

    if (instance == NULL) {
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    role = otThreadGetDeviceRole(instance);
    rloc16 = otThreadGetRloc16(instance);
    extaddr = otLinkGetExtendedAddress(instance);
    esp_openthread_lock_release();

    if (extaddr == NULL) {
        ESP_LOGI(TAG, "Thread role=%s rloc16=0x%04x", thread_role_to_str(role), rloc16);
        return;
    }

    ESP_LOGI(TAG,
             "Thread role=%s rloc16=0x%04x extaddr=%02x%02x%02x%02x%02x%02x%02x%02x",
             thread_role_to_str(role),
             rloc16,
             extaddr->m8[0], extaddr->m8[1], extaddr->m8[2], extaddr->m8[3],
             extaddr->m8[4], extaddr->m8[5], extaddr->m8[6], extaddr->m8[7]);
}

static void start_thread_from_active_dataset(void)
{
    otOperationalDatasetTlvs dataset = {0};
    otError error;

    esp_openthread_lock_acquire(portMAX_DELAY);
    error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));
    esp_openthread_lock_release();
}

static int read_battery_mv(void)
{
    int raw_mv = 0;
    int batt_mv = 0;

    if (s_adc_handle == NULL) {
        return -1;
    }

    if (s_adc_cali_ready &&
        adc_oneshot_get_calibrated_result(s_adc_handle, s_adc_cali_handle, s_adc_channel, &raw_mv) == ESP_OK) {
        batt_mv = raw_mv;
    } else {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) != ESP_OK) {
            return -1;
        }
        batt_mv = raw;
    }

    batt_mv = batt_mv * (CONFIG_SOILSENSE_BATT_DIVIDER_R1_OHM + CONFIG_SOILSENSE_BATT_DIVIDER_R2_OHM) /
              CONFIG_SOILSENSE_BATT_DIVIDER_R2_OHM;
    return batt_mv;
}

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

static size_t build_health_cbor(uint8_t *buf, size_t max_len, int batt_mv, int rssi_dbm, int64_t uptime_s,
                                uint16_t rloc16, const char *role)
{
    size_t idx = 0;
    if (max_len < 64) {
        return 0;
    }

    buf[idx++] = 0xA5;
    idx = cbor_put_tstr(buf, idx, "batt");
    idx = cbor_put_uint(buf, idx, batt_mv > 0 ? (uint64_t)batt_mv : 0);
    idx = cbor_put_tstr(buf, idx, "rssi");
    idx = (rssi_dbm >= 0) ? cbor_put_uint(buf, idx, (uint64_t)rssi_dbm) : cbor_put_nint(buf, idx, (int64_t)rssi_dbm);
    idx = cbor_put_tstr(buf, idx, "up");
    idx = cbor_put_uint(buf, idx, uptime_s > 0 ? (uint64_t)uptime_s : 0);
    idx = cbor_put_tstr(buf, idx, "rloc16");
    idx = cbor_put_uint(buf, idx, rloc16);
    idx = cbor_put_tstr(buf, idx, "role");
    idx = cbor_put_tstr(buf, idx, role);
    return idx;
}

static void send_health_response(otInstance *instance, const otMessage *request, const otMessageInfo *message_info)
{
    int batt_mv = read_battery_mv();
    int rssi_dbm = -127;
    int8_t parent_rssi = -127;
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;
    uint16_t rloc16 = 0;
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    uint8_t payload[64];
    size_t payload_len;
    otMessage *response = NULL;
    otError error;
    const char *role_str = "disabled";

    esp_openthread_lock_acquire(portMAX_DELAY);
    role = otThreadGetDeviceRole(instance);
    rloc16 = otThreadGetRloc16(instance);
    if (otThreadGetParentAverageRssi(instance, &parent_rssi) == OT_ERROR_NONE) {
        rssi_dbm = parent_rssi;
    } else if (otThreadGetParentLastRssi(instance, &parent_rssi) == OT_ERROR_NONE) {
        rssi_dbm = parent_rssi;
    }
    esp_openthread_lock_release();

    switch (role) {
    case OT_DEVICE_ROLE_CHILD:
        role_str = "child";
        break;
    case OT_DEVICE_ROLE_ROUTER:
        role_str = "router";
        break;
    case OT_DEVICE_ROLE_LEADER:
        role_str = "leader";
        break;
    case OT_DEVICE_ROLE_DETACHED:
        role_str = "detached";
        break;
    default:
        role_str = "disabled";
        break;
    }

    payload_len = build_health_cbor(payload, sizeof(payload), batt_mv, rssi_dbm, uptime_s, rloc16, role_str);
    if (payload_len == 0) {
        return;
    }

    response = otCoapNewMessage(instance, NULL);
    if (response == NULL) {
        return;
    }

    error = otCoapMessageInitResponse(response, request, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_CONTENT);
    if (error != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }
    if (otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR) != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }
    if (otCoapMessageSetPayloadMarker(response) != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }
    if (otMessageAppend(response, payload, (uint16_t)payload_len) != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }

    ESP_LOGI(TAG, "health batt=%d mV role=%s up=%lld s", batt_mv, role_str, (long long)uptime_s);
    if (otCoapSendResponse(instance, response, message_info) != OT_ERROR_NONE) {
        otMessageFree(response);
    }
}

static void coap_health_handler(void *context, otMessage *message, const otMessageInfo *message_info)
{
    otInstance *instance = context;
    uint8_t code = otCoapMessageGetCode(message);

    if (code != OT_COAP_CODE_GET) {
        return;
    }
    send_health_response(instance, message, message_info);
}

static void init_adc(void)
{
    const int adc_gpio = CONFIG_SOILSENSE_BATT_ADC_IO;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(adc_gpio, &s_adc_unit_id, &s_adc_channel));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg));

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = s_adc_unit_id,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali_handle) == ESP_OK) {
        s_adc_cali_ready = true;
    }
#endif
}

static void ot_task_worker(void *arg)
{
    esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

    ESP_ERROR_CHECK(esp_openthread_start(&config));
    esp_netif_set_default_netif(esp_openthread_get_netif());

#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    esp_openthread_lock_acquire(portMAX_DELAY);
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
    esp_openthread_lock_release();
#endif
    start_thread_from_active_dataset();
    force_child_role();
    ESP_ERROR_CHECK(otCoapStart(esp_openthread_get_instance(), COAP_PORT));
    s_health_resource.mUriPath = HEALTH_URI;
    s_health_resource.mHandler = coap_health_handler;
    s_health_resource.mContext = esp_openthread_get_instance();
    s_health_resource.mNext = NULL;
    otCoapAddResource(esp_openthread_get_instance(), &s_health_resource);

    log_thread_identity();
    ESP_LOGI(TAG, "Battery node booted; exposing coap://[addr]/%s", HEALTH_URI);
    esp_openthread_launch_mainloop();
}

void app_main(void)
{
    ESP_LOGI(TAG, "SoilSense Battery Node v%s", esp_app_get_description()->version);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&(esp_vfs_eventfd_config_t){ .max_fds = 4 }));

    init_adc();

    if (xTaskCreate(ot_task_worker, "ot_ftd", CONFIG_OPENTHREAD_TASK_SIZE, NULL, 5, &s_ot_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start OpenThread task");
        return;
    }
}
