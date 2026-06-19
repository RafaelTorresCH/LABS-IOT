/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Command Line Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread/thread.h"

#if CONFIG_OPENTHREAD_FTD
#include "openthread/thread_ftd.h"
#endif

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

#define TAG "ot_esp_cli"

void start_coap_server(void);
void start_valve_server(void);
void start_water_level_pump(void);

static void wipe_openthread_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("nvs", "openthread", NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OpenThread NVS namespace not available yet: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Cleared persisted OpenThread settings to avoid stale dataset restore");
    } else {
        ESP_LOGW(TAG, "Could not clear OpenThread settings: %s", esp_err_to_name(err));
    }
}

static void configure_thread_child(void)
{
#if CONFIG_OPENTHREAD_FTD
    otInstance *instance = esp_openthread_get_instance();

    if (instance == NULL) {
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otThreadSetRouterEligible(instance, false);
    esp_openthread_lock_release();
#else
    ESP_LOGI(TAG, "MTD build selected; node will attach as child");
#endif
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

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

#if CONFIG_OPENTHREAD_CLI
    ot_console_start();
    ot_register_external_commands();
#endif

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

    wipe_openthread_settings();
    ESP_ERROR_CHECK(esp_openthread_start(&config));
    configure_thread_child();
    start_thread_from_active_dataset();
    ESP_LOGI(TAG, "SoilSense water profile: OpenThread child + CoAP resources /nivel and /act/valve");
    log_thread_identity();
    start_coap_server();
    start_water_level_pump();
    start_valve_server();
}
