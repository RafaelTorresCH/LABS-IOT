# Host Bridge

The battery node already exposes `GET /sys/health` over CoAP.

This bridge reads that CoAP resource from Fedora through `ot-ctl`, converts the
payload to InfluxDB line protocol, and writes it to your InfluxDB bucket.

## Run

```bash
python3 tools/coap_to_influx.py --once
```

## Continuous mode

```bash
export SOILSENSE_INFLUX_URL="http://127.0.0.1:8086"
export SOILSENSE_INFLUX_ORG="your-org"
export SOILSENSE_INFLUX_BUCKET="your-bucket"
export SOILSENSE_INFLUX_TOKEN="your-token"
python3 tools/coap_to_influx.py --interval 30
```

## What it discovers

- Mesh-local prefix from `ot-ctl dataset active`
- Remote router RLOC16 from `ot-ctl neighbor table`
- Battery telemetry from `coap://[fd11:...]/sys/health`

## Fast checks

Use the helper suite to validate the bridge and the Lab 8 hardening rules:

```bash
python3 tools/test_lab7_lab8.py lab5
python3 tools/test_lab7_lab8.py lab8
python3 tools/test_lab7_lab8.py lab7
```

If `ot-ctl` needs sudo on your machine, run `sudo -v` once before the live Lab 7 check, or set:

```bash
export SOILSENSE_THREAD_NODE_ADDR="fd11:22:33:44:0:ff:fe00:7000"
export SOILSENSE_THREAD_RLOC16="7000"
```

## OTBR recovery helper

When the Fedora host loses the Spinel session with the mini RCP, use:

```bash
sudo tools/otbr_resilience.sh port
sudo tools/otbr_resilience.sh recover
```

This is especially useful when:

- the mini board was moved between USB ports
- a previous `idf.py monitor` kept the USB Serial/JTAG endpoint busy
- the OTBR host comes back up but `ot-ctl` still returns `Connection refused`

For better service resilience on Fedora, you can also apply
[otbr-agent-override.conf.example](/home/musicunauta24/esp/LABS-IOT/LAB_07/tools/otbr-agent-override.conf.example)
as a systemd drop-in override.

## Influx measurement

Measurement: `thread_battery`

Tags:

- `node`
- `role`
- `addr`

Fields:

- `batt_mv`
- `rssi_dbm`
- `uptime_s`

## SCADA bridge

If you want to keep `sample_project-main/` unchanged, use:

```bash
python3 tools/thread_to_scada_bridge.py \
  --dashboard-host 192.168.1.50 \
  --sensor-addr fd11:22:33:44:0:ff:fe00:7001 \
  --water-addr fd11:22:33:44:0:ff:fe00:7002
```

What it does:

- polls `GET /env/temp` from the Thread sensor node
- polls `GET /nivel` from the Thread water node
- builds the JSON expected by `sample_project-main`
- sends `POST coap://<dashboard-host>/sensores`

Recommended rollout:

1. Simulate payload creation without Thread nodes:

```bash
python3 tools/thread_to_scada_bridge.py --simulate --dry-run --once
```

2. Validate real CoAP reads without touching the dashboard:

```bash
python3 tools/thread_to_scada_bridge.py \
  --sensor-addr fd11:22:33:44:0:ff:fe00:7001 \
  --water-addr fd11:22:33:44:0:ff:fe00:7002 \
  --dry-run --once
```

3. Send live JSON to the dashboard:

```bash
set -a
. tools/scada_bridge.env.example
set +a
python3 tools/thread_to_scada_bridge.py \
  --dashboard-host "$SOILSENSE_SCADA_HOST" \
  --dashboard-port "$SOILSENSE_SCADA_PORT" \
  --sensor-addr "$SOILSENSE_SENSOR_ADDR" \
  --water-addr "$SOILSENSE_WATER_ADDR" \
  --interval "$SOILSENSE_SCADA_INTERVAL_S"
```

Dashboard JSON shape:

```json
{
  "hum_a": 61,
  "hum_s": 73,
  "level": 88
}
```

The bridge also adds optional fields such as `temp_c` and `pump`, but the
sample dashboard safely ignores any extra keys.

Supported sensor payload keys:

- Sensor node:
  - preferred: `t_x10`, `h_x10`, `soil_x10`
  - accepted fallback: `temp_c`, `temperature_c`, `air_humidity_pct`, `hum_a`, `soil_humidity_pct`, `hum_s`
- Water node:
  - preferred: `level`, `raw`, `pump`
  - accepted fallback: `level_x10`, `water_level_pct`

Sanitization rules:

- humidity values are clamped to `0..100`
- level is clamped to `0..100`
- pump/raw states are normalized to `0` or `1`
- `--no-meta` sends only `hum_a`, `hum_s`, `level`

Quick unit check:

```bash
python3 tools/test_scada_bridge.py
```

Local end-to-end structure check:

```bash
python3 tools/test_thread_dashboard_e2e.py
```

This E2E check proves the exact project structure:

- sensor-side values are represented as Thread/CoAP telemetry
- the host bridge transforms them into dashboard JSON
- the dashboard-side CoAP endpoint receives `POST /sensores`
