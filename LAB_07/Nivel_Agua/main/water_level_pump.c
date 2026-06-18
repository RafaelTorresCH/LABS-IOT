#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "sensor_gateway_config.h"

static const char *TAG = "nivel_agua";

#define WATER_STABLE_SAMPLES 3

static bool s_pump_on;

static bool water_detected(void)
{
    int level = gpio_get_level((gpio_num_t)LAB2_WATER_LEVEL_GPIO);
    return level == LAB2_WATER_SENSOR_ACTIVE_LEVEL;
}

static void pump_set(bool on)
{
    int gpio_level = on ? LAB2_PUMP_ACTIVE_LEVEL : !LAB2_PUMP_ACTIVE_LEVEL;
    gpio_set_level((gpio_num_t)LAB2_PUMP_CONTROL_GPIO, gpio_level);
    s_pump_on = on;
    ESP_LOGI(TAG, "Motobomba %s (GPIO%d=%d)", on ? "ENCENDIDA" : "APAGADA",
             LAB2_PUMP_CONTROL_GPIO, gpio_level);
}

static void water_level_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t wet_count = 0;
    uint8_t dry_count = 0;

    pump_set(false);

    while (1) {
        bool wet = water_detected();

        if (wet) {
            wet_count++;
            dry_count = 0;
        } else {
            dry_count++;
            wet_count = 0;
        }

        if (dry_count >= WATER_STABLE_SAMPLES && !s_pump_on) {
            pump_set(true);
        } else if (wet_count >= WATER_STABLE_SAMPLES && s_pump_on) {
            pump_set(false);
        }

        ESP_LOGI(TAG, "Sensor nivel GPIO%d=%d | Agua=%s | Bomba=%s",
                 LAB2_WATER_LEVEL_GPIO,
                 gpio_get_level((gpio_num_t)LAB2_WATER_LEVEL_GPIO),
                 wet ? "DETECTADA" : "NO DETECTADA",
                 s_pump_on ? "ON" : "OFF");

        vTaskDelay(pdMS_TO_TICKS(LAB2_WATER_SAMPLE_PERIOD_MS));
    }
}

void start_water_level_pump(void)
{
    gpio_config_t sensor_io = {
        .pin_bit_mask = 1ULL << LAB2_WATER_LEVEL_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sensor_io));

    gpio_config_t pump_io = {
        .pin_bit_mask = 1ULL << LAB2_PUMP_CONTROL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pump_io));

    ESP_LOGI(TAG, "Nivel de agua iniciado: sensor=GPIO%d active=%d bomba=GPIO%d active=%d",
             LAB2_WATER_LEVEL_GPIO,
             LAB2_WATER_SENSOR_ACTIVE_LEVEL,
             LAB2_PUMP_CONTROL_GPIO,
             LAB2_PUMP_ACTIVE_LEVEL);

    xTaskCreate(water_level_task, "water_level", 4096, NULL, 5, NULL);
}

