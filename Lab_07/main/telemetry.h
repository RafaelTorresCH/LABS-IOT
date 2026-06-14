typedef struct
{
    int batt;
    int rssi;
    uint32_t up;
} telemetry_t;

void telemetry_get(telemetry_t *t);