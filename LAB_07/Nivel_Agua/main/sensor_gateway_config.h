#pragma once

/*
 * Lab2 scope: ESP32 gateway between Wi-Fi/MQTT sensor nodes and the
 * OpenThread border router owned by the other team member.
 *
 * Override these values before flashing the gateway.
 */

#ifndef LAB2_WIFI_SSID
#define LAB2_WIFI_SSID "TU_WIFI"
#endif

#ifndef LAB2_WIFI_PASSWORD
#define LAB2_WIFI_PASSWORD "TU_PASSWORD"
#endif

#ifndef LAB2_MQTT_BROKER_URI
#define LAB2_MQTT_BROKER_URI "mqtt://192.168.1.10:1883"
#endif

#ifndef LAB2_BORDER_ROUTER_HOST
#define LAB2_BORDER_ROUTER_HOST "fdde:ad00:beef:0::1"
#endif

#ifndef LAB2_BORDER_ROUTER_COAP_PORT
#define LAB2_BORDER_ROUTER_COAP_PORT "5683"
#endif

#ifndef LAB2_MQTT_TELEMETRY_TOPIC
#define LAB2_MQTT_TELEMETRY_TOPIC "lab2/sensors/+/telemetry"
#endif

#ifndef LAB2_MQTT_ACTUATOR_COMMAND_TOPIC
#define LAB2_MQTT_ACTUATOR_COMMAND_TOPIC "lab2/actuators/+/+/set"
#endif

/*
 * ESP32-C6 pin assignment used by the sensor/actuator nodes.
 * The gateway firmware does not read these pins directly, but keeping them
 * here makes the hardware contract visible from one place.
 */
#ifndef LAB2_DHT_DATA_GPIO
#define LAB2_DHT_DATA_GPIO 18
#endif

#ifndef LAB2_SOIL_MOISTURE_ADC_GPIO
#define LAB2_SOIL_MOISTURE_ADC_GPIO 0
#endif

#ifndef LAB2_WATER_LEVEL_GPIO
#define LAB2_WATER_LEVEL_GPIO 20
#endif

#ifndef LAB2_PUMP_CONTROL_GPIO
#define LAB2_PUMP_CONTROL_GPIO 8
#endif

#ifndef LAB2_WATER_SENSOR_ACTIVE_LEVEL
#define LAB2_WATER_SENSOR_ACTIVE_LEVEL 1
#endif

#ifndef LAB2_PUMP_ACTIVE_LEVEL
#define LAB2_PUMP_ACTIVE_LEVEL 1
#endif

#ifndef LAB2_WATER_SAMPLE_PERIOD_MS
#define LAB2_WATER_SAMPLE_PERIOD_MS 1000
#endif

#ifndef LAB2_SENSOR_NODE_ID
#define LAB2_SENSOR_NODE_ID "nodo1"
#endif

/* Sensor type connected to LAB2_DHT_DATA_GPIO: use 11 for DHT11 or 22 for DHT22. */
#ifndef LAB2_DHT_TYPE
#define LAB2_DHT_TYPE 11
#endif

/*
 * Many soil moisture modules produce a lower ADC value when the soil is wet.
 * Keep this as 1 for the common FC-28/YL-69-style analog modules.
 */
#ifndef LAB2_SOIL_MOISTURE_INVERT
#define LAB2_SOIL_MOISTURE_INVERT 1
#endif

#ifndef LAB2_SENSOR_SAMPLE_PERIOD_MS
#define LAB2_SENSOR_SAMPLE_PERIOD_MS 5000
#endif
