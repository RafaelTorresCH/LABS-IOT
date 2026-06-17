| Supported Targets | ESP32-C5 | ESP32-C6 | ESP32-H2 |
| ----------------- | -------- | -------- | -------- |

# OpenThread Radio Co-Processor (RCP) Example

This example demonstrates an [OpenThread Radio Co-Processor](https://openthread.io/platforms/co-processor).

OpenThread RCP doesn't function alone, it needs to work together with a Host and this example covers two common user scenarios:
- Work with a Host Processor to perform a [Thread Border Router](https://openthread.io/guides/border-router).
- Work as a [Thread Sniffer](https://openthread.io/guides/pyspinel/sniffer).

## How to use example

### Hardware Required

To run this example, a board with IEEE 802.15.4 module (for example ESP32-H2) is required.

### Configure the project

The default communication interface is port 0 of ESP32-H2 UART running at 460800 baud, change the configuration in [esp_ot_config.h](main/esp_ot_config.h) if you want to use another interface or need different communication parameters.

### RCP Size Optimization Configuration

To optimize the size of the RCP firmware, the following configurations are enabled by default:

```
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT=y
CONFIG_COMPILER_SAVE_RESTORE_LIBCALLS=y
CONFIG_ESP_ERR_TO_NAME_LOOKUP=n
CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC=n
CONFIG_OPENTHREAD_LOG_LEVEL_NONE=y
```
Configure them via `idf.py menuconfig` if you need.

The firmware size are as follows (reference value):

```
                                Before Optimization     After Optimization

esp_ot_rcp.bin for esp32h2          314KB                   184KB

esp_ot_rcp.bin for esp32c6          299KB                   208kb
```

### Build and Flash

Build the project and flash it to the board:

```
idf.py -p <PORT> build flash
```

Now you'll get an OpenThread RCP, you can try the following use cases:

#### Thread Border Router

Please refer to [ot_br](../ot_br) example for the setup steps.

#### Thread Sniffer

Please refer to [Thread Sniffer](https://openthread.io/guides/pyspinel/sniffer) for the detailed instructions.

## SoilSense lab profiles

This repository now carries the Lab 7 scaffolding as two profiles:

- Root project: `ot-rcp` for the mini board that acts as the RCP for the Fedora OTBR host.
- `ftd_battery/`: native-radio FTD battery node with `GET /sys/health` over CoAP.

## Host bridge to InfluxDB

Once the OTBR and the battery node are attached to the same Thread network, run:

```bash
python3 tools/coap_to_influx.py --once
```

or keep it running:

```bash
export SOILSENSE_INFLUX_URL="http://127.0.0.1:8086"
export SOILSENSE_INFLUX_ORG="your-org"
export SOILSENSE_INFLUX_BUCKET="your-bucket"
export SOILSENSE_INFLUX_TOKEN="your-token"
python3 tools/coap_to_influx.py --interval 30
```

The bridge auto-discovers the battery node from the OTBR router/neighbor tables,
polls `GET /sys/health`, and writes `thread_battery` points into InfluxDB.

## Project Report

### Scope

This workspace implements the Lab 7 and Lab 8 backbone for SoilSense while keeping the sensor nodes and dashboard logic separate. The current deliverable focuses on:

- a Thread Border Router host on Fedora
- a mini board acting as the OpenThread RCP
- a separate ESP32-C6 FTD battery node
- a host-side CoAP bridge that exports telemetry to InfluxDB
- automated checks for the lab requirements

### Design decisions

- Two firmware profiles are kept separate because the radio co-processor and the battery node have different responsibilities, flash targets, and serial ports.
- The RCP is flashed to `/dev/ttyACM0` and only serves as radio hardware for the OTBR host.
- The battery node is flashed to `/dev/ttyACM1` and exposes `GET /sys/health` over CoAP.
- The battery node reports a small CBOR map with `batt`, `rssi`, `up`, `rloc16`, and `role`. This keeps the payload compact and easy to consume from a bridge.
- The InfluxDB path is host-side on Fedora. That keeps the mesh simple and makes the bridge reusable for later dashboard work.
- Secrets are kept out of version control through `secrets.h` plus `secrets.h.example`.
- The repository includes a test helper so you can validate Lab 7 and Lab 8 repeatedly without manually reconstructing each command.

### Current architecture

- `main/`: OpenThread RCP firmware for the mini board.
- `ftd_battery/`: native OpenThread FTD battery node.
- `ot-br-posix/`: OTBR host stack on Fedora.
- `tools/coap_to_influx.py`: bridge from Thread CoAP telemetry to InfluxDB line protocol.
- `tools/test_lab7_lab8.py`: quick verification suite for the lab milestones.

### Verification flow

1. Build and flash the mini RCP board from `main/`.
2. Build and flash the battery node from `ftd_battery/`.
3. Start `otbr-agent` on Fedora and confirm the Thread topology shows the router and the FTD child/router.
4. Run the bridge once:

```bash
python3 tools/coap_to_influx.py --once
```

5. Run the automated checks:

```bash
python3 tools/test_lab7_lab8.py lab8
python3 tools/test_lab7_lab8.py lab7
```

If the live Lab 7 check needs sudo for `ot-ctl`, run `sudo -v` first in your terminal session.

### Notes on the sample project

The `sample_project-main/` folder is a separate SCADA-style dashboard example. It is useful as a reference for UI behavior, but it is not the active data path for the current Thread + InfluxDB architecture.

Build each profile from its own directory with its own `idf.py build`.
