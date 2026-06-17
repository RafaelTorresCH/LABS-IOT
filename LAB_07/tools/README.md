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
