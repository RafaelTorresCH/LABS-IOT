| Supported Targets | ESP32-C5 | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- | -------- |

# OpenThread Command Line Example

This example demonstrates an [OpenThread CLI](https://github.com/openthread/openthread/blob/master/src/cli/README.md), with some additional features such as TCP, UDP and Iperf.

## How to use example

### Hardware Required

To run this example, a board with IEEE 802.15.4 module (for example ESP32-H2) is required.

### Configure the project

```
idf.py menuconfig
```

The example can run with the default configuration. OpenThread Command Line is enabled with UART as the default interface. Additionally, USB JTAG is also supported and can be activated through the menuconfig:

```
Component config → ESP System Settings → Channel for console output → USB Serial/JTAG Controller
```

### Build, Flash, and Run

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT build flash monitor
```

Now you'll get an OpenThread command line shell.

### Example Output

The `help` command will print all of the supported commands.
```bash
esp32h2> ot help
I(7058) OPENTHREAD:[INFO]-CLI-----: execute command: help
bbr
bufferinfo
ccathreshold
channel
child
childip
childmax
childsupervision
childtimeout
coap
contextreusedelay
counters
dataset
delaytimermin
diag
discover
dns
domainname
eidcache
eui64
extaddr
extpanid
factoryreset
...
```

## Set Up Network

To run this example, at least two ESP32-H2 boards flashed with this ot_cli example are required.

On the first device, run the following commands:
```bash
esp32h2> ot factoryreset
... # the device will reboot

esp32h2> ot dataset init new
Done
esp32h2> ot dataset commit active
Done
esp32h2> ot ifconfig up
Done
esp32h2> ot thread start
Done

# After some seconds

esp32h2> ot state
leader
Done
```
Now the first device has formed a Thread network as a leader. Get some information which will be used in next steps:
```bash
esp32h2> ot ipaddr
fdde:ad00:beef:0:0:ff:fe00:fc00
fdde:ad00:beef:0:0:ff:fe00:8000
fdde:ad00:beef:0:a7c6:6311:9c8c:271b
fe80:0:0:0:5c27:a723:7115:c8f8

# Get the Active Dataset
esp32h2> ot dataset active -x
0e080000000000010000000300001835060004001fffe00208fe7bb701f5f1125d0708fd75cbde7c6647bd0510b3914792d44f45b6c7d76eb9306eec94030f4f70656e5468726561642d35383332010258320410e35c581af5029b054fc904a24c2b27700c0402a0fff8
```

On the second device, set the active dataset from leader, and start Thread interface:
```bash
esp32h2> ot factoryreset
... # the device will reboot

esp32h2> ot dataset set active 0e080000000000010000000300001835060004001fffe00208fe7bb701f5f1125d0708fd75cbde7c6647bd0510b3914792d44f45b6c7d76eb9306eec94030f4f70656e5468726561642d35383332010258320410e35c581af5029b054fc904a24c2b27700c0402a0fff8
esp32h2> ot ifconfig up
Done
esp32h2> ot thread start
Done

# After some seconds

esp32h2> ot state
router  # child is also a valid state
Done
```
The second device has joined the Thread network as a router (or a child).

## Extension commands

You can refer to the [extension command](https://github.com/espressif/esp-thread-br/blob/main/components/esp_ot_cli_extension/README.md) about the extension commands.

The following examples are supported by `ot_cli`:

* TCP and UDP Example

## Using iPerf to measure bandwidth

iPerf is a tool used to obtain TCP or UDP throughput on the Thread network. To run iPerf, you need to have two Thread devices on the same network.

Refer to [the iperf-cmd component](https://components.espressif.com/components/espressif/iperf-cmd) for details on specific configurations.

### Typical usage on a thread network

For measuring the TCP throughput, first create an iperf service on one node:
```bash
> iperf -V -s -t 20 -i 3 -p 5001 -f k
Done
```

Then create an iperf client connecting to the service on another node. Note that the [ML-EID](https://openthread.io/guides/thread-primer/ipv6-addressing#unicast_address_types) address is used for iperf.

```bash
> ipaddr mleid
fdde:ad00:beef:0:a7c6:6311:9c8c:271b
Done

> iperf -V -c fdde:ad00:beef:0:a7c6:6311:9c8c:271b -t 20 -i 1 -p 5001 -l 85 -f k
Done
[ ID] Interval		Transfer	Bandwidth
[  1]  0.0- 1.0 sec	3.15 KBytes	25.16 Kbits/sec
[  1]  1.0- 2.0 sec	2.89 KBytes	23.12 Kbits/sec
[  1]  2.0- 3.0 sec	2.98 KBytes	23.80 Kbits/sec
...
[  1]  9.0-10.0 sec	2.55 KBytes	20.40 Kbits/sec
[  1]  0.0-10.0 sec	27.80 KBytes	22.24 Kbits/sec
```

For measuring the UDP throughput, first create an iperf service similarly:

```bash
> iperf -V -u -s -t 20 -i 3 -p 5001 -f k
Done
```

Then create an iperf client:
```bash
> iperf -V -u -c fdde:ad00:beef:0:a7c6:6311:9c8c:271b -t 20 -i 1 -p 5001 -l 85 -f k
Done
```

## Project Report

### Scope

This workspace implements the Lab 5, Lab 7 and Lab 8 backbone for SoilSense while keeping the firmware roles separated and the dashboard reusable. The current deliverable focuses on:

- an OpenThread Border Router deployment path on Fedora
- a Thread Border Router host on Fedora
- a mini board acting as the OpenThread RCP
- a separate ESP32-C6 FTD battery node
- a host-side bridge layer for both InfluxDB and the SCADA dashboard
- a dashboard-facing JSON payload compatible with `sample_project-main/`
- automated checks for the lab requirements

### Design decisions

- Two firmware profiles are kept separate because the radio co-processor and the battery node have different responsibilities, flash targets, and serial ports.
- The RCP is flashed to `/dev/ttyACM0` and only serves as radio hardware for the OTBR host.
- The battery node is flashed to `/dev/ttyACM1` and exposes `GET /sys/health` over CoAP.
- The OTBR remains the boundary between the proximity network (Thread) and the access network (Wi-Fi/Ethernet), which is the core architectural concern of Lab 5.
- The battery node reports a small CBOR map with `batt`, `rssi`, `up`, `rloc16`, and `role`. This keeps the payload compact and easy to consume from a bridge.
- The sensor and actuator nodes are polled from Fedora over Thread CoAP, not through custom dashboard firmware changes. This keeps `sample_project-main/` intact.
- The SCADA integration is performed by a host-side adapter that converts CoAP + CBOR telemetry into the JSON that the dashboard already expects on `POST /sensores`.
- The InfluxDB path stays on the dashboard side for historical storage, so the bridge only needs to deliver valid JSON to the SCADA endpoint.
- Secrets are kept out of version control through `secrets.h` plus `secrets.h.example`.
- The repository includes a test helper so you can validate Lab 7 and Lab 8 repeatedly without manually reconstructing each command.

### Current architecture

- `main/`: OpenThread RCP firmware for the mini board.
- `ftd_battery/`: native OpenThread FTD battery node.
- `Sensores/`: Thread sensor node exposing `GET /env/temp`.
- `Nivel_Agua/`: Thread water-level / pump node exposing `GET /nivel`.
- `ot-br-posix/`: OTBR host stack on Fedora.
- `tools/coap_to_influx.py`: bridge from Thread CoAP telemetry to InfluxDB line protocol.
- `tools/thread_to_scada_bridge.py`: bridge from Thread CoAP telemetry to the dashboard CoAP API.
- `tools/test_scada_bridge.py`: unit checks for the SCADA payload mapping.
- `tools/test_lab7_lab8.py`: quick verification suite for the lab milestones.
- `sample_project-main/`: existing dashboard + CoAP receiver + InfluxDB writer kept unchanged.

### Verification flow

1. Build and flash the mini RCP board from `main/`.
2. Start `otbr-agent` on Fedora and confirm the OTBR is up, has an active dataset, and exposes prefixes and IPv6 addresses.
3. Run the Lab 5 validation suite:

```bash
python3 tools/test_lab7_lab8.py lab5
```

4. Build and flash the battery node from `ftd_battery/`.
5. Confirm the Thread topology shows the router and the FTD child/router.
6. Run the battery bridge once if you want to validate the Lab 7 telemetry path:

```bash
python3 tools/coap_to_influx.py --once
```

7. Run the dashboard bridge once if you want to validate the SCADA path without changing `sample_project-main/`:

```bash
python3 tools/thread_to_scada_bridge.py \
  --dashboard-host <IP_DEL_DEVICE_CON_DASHBOARD> \
  --sensor-addr <IPv6_SENSOR_THREAD> \
  --water-addr <IPv6_WATER_THREAD> \
  --once
```

### OTBR resilience notes

- Always bind the OTBR radio using `/dev/serial/by-id/...Espressif...`, not raw `/dev/ttyACM*`.
- Do not keep `idf.py monitor` attached to the mini board while `otbr-agent` is running, because both processes contend for the same USB Serial/JTAG endpoint.
- If the OTBR suddenly starts failing with `connect session failed: Connection refused` or `Platform------: Init() ... Failure`, the first suspicion should be serial ownership or a renamed USB port, not the Thread dataset.

Quick recovery flow:

```bash
sudo tools/otbr_resilience.sh port
sudo tools/otbr_resilience.sh recommend
sudo tools/otbr_resilience.sh recover
```

What this helper checks:

- whether the Espressif USB serial-by-id device is currently present
- whether another process is holding the radio port open
- whether `otbr-agent` restarts cleanly and `ot-ctl state` becomes reachable

Recommended systemd hardening:

1. Copy [tools/otbr-agent-override.conf.example](/home/musicunauta24/esp/LABS-IOT/LAB_07/tools/otbr-agent-override.conf.example) into `/etc/systemd/system/otbr-agent.service.d/override.conf`.
2. Optionally enable the `ExecStartPre` line with your exact `/dev/serial/by-id/...` path.
3. Run `sudo systemctl daemon-reload && sudo systemctl restart otbr-agent`.

Before going live, you can validate the payload generation without needing the
real nodes:

```bash
python3 tools/thread_to_scada_bridge.py --simulate --dry-run --once
```

You can also validate real Thread reads without sending anything to the
dashboard:

```bash
python3 tools/thread_to_scada_bridge.py \
  --sensor-addr <IPv6_SENSOR_THREAD> \
  --water-addr <IPv6_WATER_THREAD> \
  --dry-run --once
```

This sends a JSON document like:

```json
{
  "hum_a": 61,
  "hum_s": 73,
  "level": 88
}
```

8. Run the automated checks:

```bash
python3 tools/test_lab7_lab8.py lab5
python3 tools/test_lab7_lab8.py lab8
python3 tools/test_lab7_lab8.py lab7
python3 tools/test_scada_bridge.py
python3 tools/test_thread_dashboard_e2e.py
```

If the live Lab 7 check needs sudo for `ot-ctl`, run `sudo -v` first in your terminal session.

### Notes on the sample project

The `sample_project-main/` folder is the dashboard-side receiver for the current SCADA integration path. It already accepts CoAP `POST /sensores`, updates the web UI in real time, and writes historical data to InfluxDB. For that reason, the integration work in this repository is done outside that folder through `tools/thread_to_scada_bridge.py`, so the dashboard code can stay unchanged.

The dashboard currently expects these JSON keys:

- `hum_a`: air humidity
- `hum_s`: soil humidity
- `level`: water level

Those values are generated from Thread CoAP telemetry as follows:

- `GET /env/temp` on the sensor node provides `h_x10` and `soil_x10`
- `GET /nivel` on the water node provides `level`
- the host bridge rounds and remaps them into the dashboard JSON

The bridge also accepts alternate keys such as `temp_c`,
`temperature_c`, `air_humidity_pct`, `soil_humidity_pct`, and
`water_level_pct`, then sanitizes the values before forwarding:

- humidity is clamped to `0..100`
- water level is clamped to `0..100`
- optional metadata such as `temp_c`, `pump`, source addresses and
  timestamps is added by default
- `--no-meta` sends only the three keys required by the dashboard

Build each profile from its own directory with its own `idf.py build`.
