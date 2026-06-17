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
python3 tools/test_lab7_lab8.py lab8
python3 tools/test_lab7_lab8.py lab7
```

If `ot-ctl` needs sudo on your machine, run `sudo -v` once before the live Lab 7 check, or set:

```bash
export SOILSENSE_THREAD_NODE_ADDR="fd11:22:33:44:0:ff:fe00:7000"
export SOILSENSE_THREAD_RLOC16="7000"
```

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
