#pragma once

#include <stdbool.h>

typedef struct {
    bool valid;
    float temperature_c;
    float air_humidity_pct;
    int soil_raw;
    float soil_pct;
} sensor_reading_t;

sensor_reading_t read_sensors(void);
