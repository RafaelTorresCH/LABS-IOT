# SoilSense FTD Battery Profile

This directory is the second profile for Lab 7 / Lab 8.

## What it does

- Joins the Thread network as a native-radio FTD.
- Reads battery voltage from an ADC divider.
- Exposes `GET /sys/health` as a CoAP resource with CBOR payload.
- Reports `batt`, `rssi`, `up`, and `role`.

## Build

```bash
cd ftd_battery
source /home/musicunauta24/.espressif/v6.0/esp-idf/export.sh
idf.py build
```

## Configure

- `SOILSENSE_BATT_ADC_IO`: GPIO connected to the divider midpoint.
- `SOILSENSE_BATT_DIVIDER_R1_OHM` / `SOILSENSE_BATT_DIVIDER_R2_OHM`: divider values in ohms.
- `SOILSENSE_BATT_REPORT_PERIOD_S`: future hook for periodic telemetry.

## Recommended battery front-end

- Use `GPIO4` first for the ADC input.
- If `GPIO4` is already occupied on your board, use `GPIO5`.
- For a single Li-ion/LiPo cell, your `82.5k` / `82.5k` pair is a good fit and gives a 2:1 divider.
- If you want lower idle drain later, `100k` / `100k` or `220k` / `220k` also works, but add a small capacitor from the ADC node to GND.
- Prefer `1%` tolerance resistors, 0603 or 0805 package, and match both resistors from the same series if possible.

## Serial ports you told me

- `FTD battery node`: `/dev/ttyACM1`
- `OTBR mini board`: `/dev/ttyACM0`

## Secret handling

Copy `main/secrets.h.example` to `main/secrets.h` if you want to reuse the lab PSK values.
