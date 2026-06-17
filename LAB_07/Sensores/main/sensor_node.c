#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "sensor_gateway_config.h"

static const char *TAG = "sensor_node";

#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1
#define ADC_MAX_RAW 4095.0f
#define DHT_TIMEOUT_US 1000
#define DHT_MAX_RETRIES 3

static EventGroupHandle_t s_event_group;
static esp_mqtt_client_handle_t s_mqtt_client;
static adc_oneshot_unit_handle_t s_adc_handle;

typedef struct {
    bool valid;
    float temperature_c;
    float air_humidity_pct;
} dht_reading_t;

typedef struct {
    dht_reading_t dht;
    int soil_raw;
    int water_raw;
    float soil_pct;
    float water_pct;
} sensor_reading_t;

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

static dht_reading_t dht_read(void)
{
    dht_reading_t reading = {0};
    uint8_t data[5] = {0};
    gpio_num_t gpio = (gpio_num_t)LAB2_DHT_DATA_GPIO;

    gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(gpio, 1);
    esp_rom_delay_us(40);
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
            return reading;
        }

        if (reading.valid) {
            ESP_LOGW(TAG, "DHT invalid range attempt %d/%d: temp=%.1f C humidity=%.1f %%",
                     attempt, DHT_MAX_RETRIES,
                     reading.temperature_c,
                     reading.air_humidity_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    dht_reading_t invalid = {0};
    return invalid;
}

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_0, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_1, &channel_config));
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

static sensor_reading_t read_sensors(void)
{
    sensor_reading_t reading = {0};
    reading.dht = dht_read_with_retries();

    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CHANNEL_0, &reading.soil_raw));
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, ADC_CHANNEL_1, &reading.water_raw));

    reading.soil_pct = adc_raw_to_pct(reading.soil_raw, LAB2_SOIL_MOISTURE_INVERT);
    reading.water_pct = adc_raw_to_pct(reading.water_raw, false);
    return reading;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_start(void)
{
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
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    if ((esp_mqtt_event_id_t)event_id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        ESP_LOGI(TAG, "MQTT connected: %s", LAB2_MQTT_BROKER_URI);
    } else if ((esp_mqtt_event_id_t)event_id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = LAB2_MQTT_BROKER_URI,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

static void sensor_task(void *pvParameters)
{
    (void)pvParameters;
    char topic[96];
    char payload[256];

    snprintf(topic, sizeof(topic), "lab2/sensors/%s/telemetry", LAB2_SENSOR_NODE_ID);

    while (1) {
        sensor_reading_t reading = read_sensors();

        if (reading.dht.valid) {
            ESP_LOGI(TAG,
                     "Mediciones nodo=%s | Temp=%.1f C | Humedad aire=%.1f %% | Humedad suelo=%.1f %% raw=%d | Agua=%.1f %% raw=%d",
                     LAB2_SENSOR_NODE_ID,
                     reading.dht.temperature_c,
                     reading.dht.air_humidity_pct,
                     reading.soil_pct,
                     reading.soil_raw,
                     reading.water_pct,
                     reading.water_raw);
        } else {
            ESP_LOGW(TAG,
                     "Mediciones nodo=%s | DHT sin lectura | Humedad suelo=%.1f %% raw=%d | Agua=%.1f %% raw=%d",
                     LAB2_SENSOR_NODE_ID,
                     reading.soil_pct,
                     reading.soil_raw,
                     reading.water_pct,
                     reading.water_raw);
        }

        snprintf(payload, sizeof(payload),
                 "{\"node\":\"%s\",\"temperature_c\":%.1f,\"air_humidity_pct\":%.1f,"
                 "\"soil_humidity_pct\":%.1f,\"soil_raw\":%d,"
                 "\"water_level_pct\":%.1f,\"water_raw\":%d}",
                 LAB2_SENSOR_NODE_ID,
                 reading.dht.valid ? reading.dht.temperature_c : -999.0f,
                 reading.dht.valid ? reading.dht.air_humidity_pct : -1.0f,
                 reading.soil_pct,
                 reading.soil_raw,
                 reading.water_pct,
                 reading.water_raw);

        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if ((bits & MQTT_CONNECTED_BIT) && s_mqtt_client) {
            int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "MQTT pub id=%d topic=%s payload=%s", msg_id, topic, payload);
        } else {
            ESP_LOGW(TAG, "MQTT not connected yet; payload only shown on serial");
        }

        vTaskDelay(pdMS_TO_TICKS(LAB2_SENSOR_SAMPLE_PERIOD_MS));
    }
}

void start_sensor_mqtt_node(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_event_group = xEventGroupCreate();
    dht_gpio_init();
    adc_init();
    wifi_start();
    mqtt_start();

    ESP_LOGI(TAG, "Sensor node '%s' pins: DHT=GPIO%d soil=GPIO%d water=GPIO%d pump=GPIO%d",
             LAB2_SENSOR_NODE_ID,
             LAB2_DHT_DATA_GPIO,
             LAB2_SOIL_MOISTURE_ADC_GPIO,
             LAB2_WATER_LEVEL_GPIO,
             LAB2_PUMP_CONTROL_GPIO);

    xTaskCreate(sensor_task, "sensor_task", 6144, NULL, 5, NULL);
}
