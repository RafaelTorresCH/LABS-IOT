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
#include "nvs_flash.h"
#include "ot_examples_common.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

#define TAG "ot_esp_cli"

// Forward declaration — implemented in coap_demo.c
void start_coap_server(void);
// Forward declaration — implemented in valve_demo.c
void start_valve_server(void);
void start_sensor_gateway(void);
void start_sensor_mqtt_node(void);

#define SOILSENSE_ROLE_CLIENT 0
#define SOILSENSE_ROLE_SENSOR 1
#define SOILSENSE_ROLE_VALVE  2
#define SOILSENSE_ROLE_GATEWAY 3
#define SOILSENSE_ROLE_SENSOR_MQTT 4

// Change this before flashing each board:
// Node A: SOILSENSE_ROLE_SENSOR, Node B: SOILSENSE_ROLE_CLIENT, Node V: SOILSENSE_ROLE_VALVE.
#define SOILSENSE_NODE_ROLE SOILSENSE_ROLE_SENSOR_MQTT

void app_main(void)
{
#if SOILSENSE_NODE_ROLE == SOILSENSE_ROLE_GATEWAY
    ESP_LOGI(TAG, "SoilSense gateway role selected; MQTT -> CoAP bridge enabled");
    start_sensor_gateway();
    return;
#elif SOILSENSE_NODE_ROLE == SOILSENSE_ROLE_SENSOR_MQTT
    ESP_LOGI(TAG, "SoilSense sensor MQTT role selected; local measurements enabled");
    start_sensor_mqtt_node();
    return;
#endif

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

    ESP_ERROR_CHECK(esp_openthread_start(&config));
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif
#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif
#if CONFIG_OPENTHREAD_NETWORK_AUTO_START
    ot_network_auto_start();
#endif
#if SOILSENSE_NODE_ROLE == SOILSENSE_ROLE_SENSOR
    start_coap_server();
#elif SOILSENSE_NODE_ROLE == SOILSENSE_ROLE_VALVE
    start_valve_server();
#else
    ESP_LOGI(TAG, "SoilSense client role selected; no local CoAP server started");
#endif
}
