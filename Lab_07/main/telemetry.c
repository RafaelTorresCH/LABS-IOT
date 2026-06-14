void telemetry_get(telemetry_t *t)
{
    t->batt = 3200;
    t->rssi = -70;
    t->up = esp_timer_get_time()/1000000;
}