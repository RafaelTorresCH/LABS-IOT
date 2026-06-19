#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/adc_types.h"

#include "sensor_profile.h"
#include "sensor_gateway_config.h"

static const char *TAG = "sensor_node";

#define ADC_MAX_RAW 4095.0f
#define DHT_TIMEOUT_US 2000
#define DHT_MAX_RETRIES 5
#define DHT_START_LOW_US 20000
#define DHT_RECOVERY_MS 50

static adc_oneshot_unit_handle_t s_adc_handle;
static bool s_sensor_hw_ready;
static adc_channel_t s_soil_channel;

typedef struct {
    bool valid;
    float temperature_c;
    float air_humidity_pct;
} dht_reading_t;
static dht_reading_t s_last_good_dht;
static bool s_has_last_good_dht;

static float clamp_pct(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 100.0f) {
        return 100.0f;
    }
    return value;
}

static bool dht_reading_is_plausible(const dht_reading_t *reading)
{
    if (!reading->valid) {
        return false;
    }

    if (reading->air_humidity_pct < 0.0f || reading->air_humidity_pct > 100.0f) {
        return false;
    }

#if LAB2_DHT_TYPE == 11
    return reading->temperature_c >= 0.0f && reading->temperature_c <= 60.0f;
#else
    return reading->temperature_c >= -40.0f && reading->temperature_c <= 80.0f;
#endif
}

static float adc_raw_to_pct(int raw, bool invert)
{
    float pct = ((float)raw / ADC_MAX_RAW) * 100.0f;
    return clamp_pct(invert ? 100.0f - pct : pct);
}

static bool adc_channel_from_gpio(int gpio, adc_channel_t *channel)
{
    switch (gpio) {
    case 0:
        *channel = ADC_CHANNEL_0;
        return true;
    case 1:
        *channel = ADC_CHANNEL_1;
        return true;
    case 2:
        *channel = ADC_CHANNEL_2;
        return true;
    case 3:
        *channel = ADC_CHANNEL_3;
        return true;
    case 4:
        *channel = ADC_CHANNEL_4;
        return true;
    case 5:
        *channel = ADC_CHANNEL_5;
        return true;
    case 6:
        *channel = ADC_CHANNEL_6;
        return true;
    default:
        return false;
    }
}

static bool wait_for_gpio_level(gpio_num_t gpio, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }
    return true;
}

static int measure_gpio_high_us(gpio_num_t gpio, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio) == 1) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

static void dht_gpio_init(void)
{
    gpio_config_t dht_io = {
        .pin_bit_mask = 1ULL << LAB2_DHT_DATA_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dht_io));
    ESP_LOGI(TAG, "DHT GPIO%d configured as INPUT_PULLUP", LAB2_DHT_DATA_GPIO);
}

static void adc_init(void)
{
    bool channel_ok = adc_channel_from_gpio(LAB2_SOIL_MOISTURE_ADC_GPIO, &s_soil_channel);
    assert(channel_ok && "LAB2_SOIL_MOISTURE_ADC_GPIO must map to ADC1 GPIO0..GPIO6 on ESP32-C6");

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_soil_channel, &channel_config));
    ESP_LOGI(TAG, "Soil ADC configured on GPIO%d -> ADC channel %d",
             LAB2_SOIL_MOISTURE_ADC_GPIO, (int)s_soil_channel);
}

static void sensor_hw_init(void)
{
    if (s_sensor_hw_ready) {
        return;
    }

    dht_gpio_init();
    adc_init();
    s_sensor_hw_ready = true;
}

static dht_reading_t dht_read(void)
{
    dht_reading_t reading = {0};
    uint8_t data[5] = {0};
    gpio_num_t gpio = (gpio_num_t)LAB2_DHT_DATA_GPIO;

    gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio, 0);
    esp_rom_delay_us(DHT_START_LOW_US);
    gpio_set_level(gpio, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);

    if (!wait_for_gpio_level(gpio, 0, DHT_TIMEOUT_US) ||
        !wait_for_gpio_level(gpio, 1, DHT_TIMEOUT_US) ||
        !wait_for_gpio_level(gpio, 0, DHT_TIMEOUT_US)) {
        ESP_LOGW(TAG, "DHT no response on GPIO%d", LAB2_DHT_DATA_GPIO);
        return reading;
    }

    for (int bit = 0; bit < 40; bit++) {
        if (!wait_for_gpio_level(gpio, 1, DHT_TIMEOUT_US)) {
            ESP_LOGW(TAG, "DHT bit %d did not start", bit);
            return reading;
        }

        int high_us = measure_gpio_high_us(gpio, DHT_TIMEOUT_US);
        if (high_us < 0) {
            ESP_LOGW(TAG, "DHT bit %d timed out", bit);
            return reading;
        }

        data[bit / 8] <<= 1;
        if (high_us > 50) {
            data[bit / 8] |= 1;
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "DHT checksum failed: got 0x%02x expected 0x%02x", data[4], checksum);
        return reading;
    }

#if LAB2_DHT_TYPE == 11
    reading.air_humidity_pct = (float)data[0] + ((float)data[1] / 10.0f);
    reading.temperature_c = (float)data[2] + ((float)data[3] / 10.0f);
#else
    uint16_t humidity_raw = ((uint16_t)data[0] << 8) | data[1];
    uint16_t temp_raw = ((uint16_t)(data[2] & 0x7f) << 8) | data[3];
    reading.air_humidity_pct = (float)humidity_raw / 10.0f;
    reading.temperature_c = (float)temp_raw / 10.0f;
    if (data[2] & 0x80) {
        reading.temperature_c *= -1.0f;
    }
#endif

    reading.valid = true;
    return reading;
}

static dht_reading_t dht_read_with_retries(void)
{
    for (int attempt = 1; attempt <= DHT_MAX_RETRIES; attempt++) {
        dht_reading_t reading = dht_read();
        if (dht_reading_is_plausible(&reading)) {
            s_last_good_dht = reading;
            s_has_last_good_dht = true;
            return reading;
        }

        if (reading.valid) {
            ESP_LOGW(TAG, "DHT invalid range attempt %d/%d: temp=%.1f C humidity=%.1f %%",
                     attempt, DHT_MAX_RETRIES,
                     reading.temperature_c,
                     reading.air_humidity_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(DHT_RECOVERY_MS));
    }

    if (s_has_last_good_dht) {
        ESP_LOGW(TAG, "DHT read failed, reusing last valid sample");
        return s_last_good_dht;
    }

    dht_reading_t invalid = {0};
    return invalid;
}

sensor_reading_t read_sensors(void)
{
    sensor_hw_init();

    sensor_reading_t reading = {0};
    dht_reading_t dht = dht_read_with_retries();
    reading.valid = dht.valid;
    reading.temperature_c = dht.temperature_c;
    reading.air_humidity_pct = dht.air_humidity_pct;

    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, s_soil_channel, &reading.soil_raw));

    reading.soil_pct = adc_raw_to_pct(reading.soil_raw, LAB2_SOIL_MOISTURE_INVERT);
    ESP_LOGI(TAG, "sample -> valid=%d temp=%.1fC hum=%.1f%% soil_raw=%d soil=%.1f%%",
             reading.valid,
             reading.temperature_c,
             reading.air_humidity_pct,
             reading.soil_raw,
             reading.soil_pct);
    return reading;
}

void start_sensor_mqtt_node(void)
{
    ESP_LOGI(TAG, "Sensor MQTT role disabled in this build; Thread child sensor mode is active instead.");
}
