#!/usr/bin/env python3
"""Fast checks for Lab 5, Lab 7 and Lab 8.

This script is meant to shorten the manual verification loop:

- Lab 5: OTBR state, active dataset, advertised prefix, topology visibility, and
  optional off-mesh CoAP reachability.
- Lab 7: Thread topology, FTD attachment, CoAP health readout, bridge payload
  decoding, dashboard payload mapping, and local end-to-end forwarding.
- Lab 8: hardening invariants that can be checked from the repo without full
  hardware orchestration.

Important: some lab requirements are inherently manual or hardware-driven
(latency measurements, NAT64 internet reachability, chaos drills, soak tests,
Wi-Fi blackout, wrong-PSK joins). This script includes automated checks for the
parts that can be verified from the repo and the current machine, and can also
print a manual checklist so nothing gets forgotten.

Usage:

    python3 tools/test_lab7_lab8.py lab5
    python3 tools/test_lab7_lab8.py lab7
    python3 tools/test_lab7_lab8.py lab8
    python3 tools/test_lab7_lab8.py all
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable, Dict, List, Sequence


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_PATH = ROOT / "tools" / "coap_to_influx.py"
SCADA_UNIT_PATH = ROOT / "tools" / "test_scada_bridge.py"
SCADA_E2E_PATH = ROOT / "tools" / "test_thread_dashboard_e2e.py"
BRIDGE_SPEC = importlib.util.spec_from_file_location("coap_to_influx", BRIDGE_PATH)
if BRIDGE_SPEC is None or BRIDGE_SPEC.loader is None:
    raise RuntimeError(f"Unable to load bridge module from {BRIDGE_PATH}")
bridge = importlib.util.module_from_spec(BRIDGE_SPEC)
sys.modules[BRIDGE_SPEC.name] = bridge
BRIDGE_SPEC.loader.exec_module(bridge)


MANUAL_CHECKLISTS: Dict[str, List[str]] = {
    "lab5": [
        "Measure 10 in-mesh CoAP RTT samples against /env/temp.",
        "Measure 10 off-mesh CoAP RTT samples through the OTBR global IPv6.",
        "Record NAT64 reachability result from a Thread node to 64:ff9b::8.8.8.8.",
        "Demonstrate that the local mesh still works when the OTBR is stopped or unplugged.",
    ],
    "lab7": [
        "Capture a screenshot or log proving fleet-level telemetry outside the mesh.",
        "Record the chosen telemetry interval and justify it against energy cost.",
        "If the course requires MQTT egress literally, run the broker-side validation and capture evidence.",
        "Perform the operator-side health validation (silent node / green-red status behavior).",
    ],
    "lab8": [
        "Run the six chaos drills: cold start, OTBR blackout, flood, random reboot, fail-safe, soak.",
        "Measure blackout recovery time and stress success ratio.",
        "Run wrong-dataset join and wrong-PSK rejection tests on real hardware.",
        "Verify fail-safe actuator behavior under parent loss with real detach conditions.",
    ],
}


class CheckError(RuntimeError):
    pass


def info(msg: str) -> None:
    print(f"[check] {msg}", flush=True)


def fail(msg: str) -> None:
    raise CheckError(msg)


def run_cmd(cmd: Sequence[str], *, cwd: Path | None = None) -> str:
    completed = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd else None,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def run_python(script: Path) -> str:
    return run_cmd(["python3", str(script)], cwd=ROOT)


def assert_true(condition: bool, msg: str) -> None:
    if not condition:
        fail(msg)


def assert_match(text: str, pattern: str, msg: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        fail(msg + f"\npattern: {pattern}\ntext:\n{text}")


def encode_cbor_int(value: int) -> bytes:
    if value >= 0:
        if value < 24:
            return bytes([value])
        if value < 256:
            return bytes([24, value])
        if value < 65536:
            return bytes([25, (value >> 8) & 0xFF, value & 0xFF])
        if value < 4294967296:
            return bytes(
                [
                    26,
                    (value >> 24) & 0xFF,
                    (value >> 16) & 0xFF,
                    (value >> 8) & 0xFF,
                    value & 0xFF,
                ]
            )
        raise ValueError("value too large")

    n = -1 - value
    if n < 24:
        return bytes([0x20 | n])
    if n < 256:
        return bytes([0x20 | 24, n])
    if n < 65536:
        return bytes([0x20 | 25, (n >> 8) & 0xFF, n & 0xFF])
    if n < 4294967296:
        return bytes(
            [
                0x20 | 26,
                (n >> 24) & 0xFF,
                (n >> 16) & 0xFF,
                (n >> 8) & 0xFF,
                n & 0xFF,
            ]
        )
    raise ValueError("negative value too small")


def encode_cbor_text(text: str) -> bytes:
    data = text.encode("utf-8")
    n = len(data)
    if n < 24:
        return bytes([0x60 | n]) + data
    if n < 256:
        return bytes([0x60 | 24, n]) + data
    if n < 65536:
        return bytes([0x60 | 25, (n >> 8) & 0xFF, n & 0xFF]) + data
    raise ValueError("text too long")


def encode_cbor_map(entries: dict[str, int | str]) -> bytes:
    items = list(entries.items())
    n = len(items)
    if n >= 24:
        raise ValueError("too many items")
    payload = bytes([0xA0 | n])
    for key, value in items:
        payload += encode_cbor_text(key)
        if isinstance(value, str):
            payload += encode_cbor_text(value)
        else:
            payload += encode_cbor_int(value)
    return payload


def test_bridge_unit_checks() -> None:
    info("unit: CBOR decoding")
    payload = encode_cbor_map(
        {
            "batt": 3210,
            "rssi": -57,
            "up": 123,
            "rloc16": 0x7000,
            "role": "router",
        }
    )
    decoded = bridge.parse_health_payload(payload)
    assert_true(decoded["batt"] == 3210, "batt should decode")
    assert_true(decoded["rssi"] == -57, "rssi should decode")
    assert_true(decoded["up"] == 123, "up should decode")
    assert_true(decoded["rloc16"] == 0x7000, "rloc16 should decode")
    assert_true(decoded["role"] == "router", "role should decode")

    info("unit: line protocol")
    telemetry = bridge.Telemetry(
        batt_mv=3210,
        rssi_dbm=-57,
        uptime_s=123,
        rloc16=0x7000,
        role="router",
        source_addr="fd11:22:33:44:0:ff:fe00:7000",
    )
    line = bridge.influx_line(telemetry)
    assert_match(line, r"^thread_battery,", "influx measurement should be correct")
    assert_match(line, r"node=0x7000", "influx node tag should exist")
    assert_match(line, r"role=router", "influx role tag should exist")
    assert_match(line, r"batt_mv=3210i", "influx battery field should exist")
    assert_match(line, r"rssi_dbm=-57i", "influx rssi field should exist")

    info("unit: Thread address derivation")
    addr = bridge.build_thread_addr("fd11:22:33:44::/64", 0x7000)
    assert_true("fd11:22:33:44" in addr and "7000" in addr, "thread address should be derived")


def test_lab5_static_checks() -> None:
    info("static: OTBR host profile")
    host_config = (ROOT / "main" / "esp_ot_config.h").read_text(encoding="utf-8")
    assert_match(host_config, r"baud_rate\s*=\s*460800", "RCP host baud rate should be fixed")
    assert_match(host_config, r"HOST_CONNECTION_MODE_RCP_UART|HOST_CONNECTION_MODE_RCP_USB", "OTBR host connection mode should exist")

    info("static: field nodes build OpenThread-only profiles")
    sensor_cmake = (ROOT / "Sensores" / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    water_cmake = (ROOT / "Nivel_Agua" / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert_match(sensor_cmake, r'"esp_ot_cli\.c"', "sensor profile should compile OpenThread bootstrap")
    assert_match(sensor_cmake, r'"coap_demo\.c"', "sensor profile should compile CoAP resource")
    assert_match(water_cmake, r'"esp_ot_cli\.c"', "water profile should compile OpenThread bootstrap")
    assert_match(water_cmake, r'"coap_demo\.c"', "water profile should compile CoAP telemetry")
    assert_match(water_cmake, r'"valve_demo\.c"', "water profile should compile CoAP valve resource")
    assert_true("sensor_gateway.c" not in sensor_cmake, "sensor profile should not compile Wi-Fi/MQTT gateway")
    assert_true("sensor_gateway.c" not in water_cmake, "water profile should not compile Wi-Fi/MQTT gateway")

    info("static: sensor resources declared")
    sensor_coap = (ROOT / "Sensores" / "main" / "coap_demo.c").read_text(encoding="utf-8")
    water_coap = (ROOT / "Nivel_Agua" / "main" / "coap_demo.c").read_text(encoding="utf-8")
    sensor_node = (ROOT / "Sensores" / "main" / "sensor_node.c").read_text(encoding="utf-8")
    sensor_contract = (ROOT / "Sensores" / "main" / "sensor_profile.h").read_text(encoding="utf-8")
    assert_match(sensor_coap, r'coap_make_str_const\("env/temp"\)', "sensor node should expose /env/temp")
    assert_match(water_coap, r'coap_make_str_const\("nivel"\)', "water node should expose /nivel")
    water_valve = (ROOT / "Nivel_Agua" / "main" / "valve_demo.c").read_text(encoding="utf-8")
    assert_match(water_valve, r'coap_make_str_const\("act/valve"\)', "water node should expose /act/valve")
    assert_match(sensor_contract, r"bool valid;", "sensor contract should expose validity flag")
    assert_match(sensor_contract, r"float air_humidity_pct;", "sensor contract should expose air humidity")
    assert_match(sensor_contract, r"float soil_pct;", "sensor contract should expose soil humidity")
    assert_match(sensor_coap, r'#include "sensor_profile\.h"', "sensor CoAP path should use shared sensor contract")
    assert_match(sensor_node, r'#include "sensor_profile\.h"', "sensor reader should use shared sensor contract")
    assert_match(sensor_node, r"adc_channel_from_gpio", "sensor profile should map ADC channel from configured GPIO")
    assert_match(sensor_node, r"DHT read failed, reusing last valid sample", "sensor profile should tolerate transient DHT failures")

    info("static: OTBR resilience assets")
    resilience_script = (ROOT / "tools" / "otbr_resilience.sh").read_text(encoding="utf-8")
    resilience_override = (ROOT / "tools" / "otbr-agent-override.conf.example").read_text(encoding="utf-8")
    setup_guide = (ROOT / "SETUP_GUIDE.md").read_text(encoding="utf-8")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    assert_match(resilience_script, r"/dev/serial/by-id/\*Espressif", "resilience helper should prefer stable by-id serial naming")
    assert_match(resilience_script, r"ot-ctl state", "resilience helper should validate ot-ctl session availability")
    assert_match(resilience_override, r"Restart=always", "systemd override should request automatic OTBR restart")
    assert_match(resilience_override, r"RestartSec=3", "systemd override should back off briefly before restart")
    assert_match(setup_guide, r"otbr_resilience\.sh recover", "setup guide should document OTBR recovery flow")
    assert_match(readme, r"OTBR resilience notes", "root README should document OTBR stability guidance")


def test_lab5_live_checks() -> None:
    info("live: OTBR operational state")
    try:
        state = bridge.run_otctl("state").strip()
    except subprocess.CalledProcessError as exc:
        raise CheckError(
            "ot-ctl is not reachable. Run `sudo tools/otbr_resilience.sh recover` "
            "and verify that no `idf.py monitor` is attached to the mini OTBR board."
        ) from exc
    assert_true(state in {"leader", "router", "child"}, f"OTBR state unexpected: {state}")

    info("live: active dataset fields")
    dataset = bridge.run_otctl("dataset", "active")
    assert_match(dataset, r"Network Name:\s+\S+", "dataset should include network name")
    assert_match(dataset, r"PAN ID:\s+0x[0-9a-fA-F]{4}", "dataset should include PAN ID")
    assert_match(dataset, r"Ext PAN ID:\s+[0-9a-fA-F]{16}", "dataset should include Ext PAN ID")
    assert_match(dataset, r"Network Key:\s+[0-9a-fA-F]{32}", "dataset should include network key")

    info("live: OTBR IPv6 addresses")
    ipaddr = bridge.run_otctl("ipaddr")
    assert_match(ipaddr, r"fe80:", "OTBR should have a link-local IPv6")
    assert_true(
        any(":" in line and not line.startswith("fe80:") for line in ipaddr.splitlines()),
        "OTBR should expose at least one non-link-local IPv6 address",
    )

    info("live: advertised prefix / netdata")
    try:
        netdata = bridge.run_otctl("netdata", "show")
    except subprocess.CalledProcessError as exc:
        fail(f"ot-ctl netdata show failed: {exc.stderr.strip() if exc.stderr else exc}")
    assert_match(netdata, r"Prefixes:", "netdata should list prefixes section")
    assert_match(netdata, r"/64", "netdata should expose at least one /64 prefix")

    info("live: topology visibility")
    child_table = bridge.run_otctl("child", "table")
    router_table = bridge.run_otctl("router", "table")
    assert_true("| ID" in router_table, "router table header should be readable")
    assert_true("| ID" in child_table, "child table header should be readable")

    manual_sensor_addr = os.environ.get("SOILSENSE_SENSOR_ADDR") or os.environ.get("SOILSENSE_THREAD_NODE_ADDR")
    if manual_sensor_addr:
        info("live: optional off-mesh sensor CoAP read")
        payload = bridge.coap_get(manual_sensor_addr, "env/temp")
        decoded = bridge.parse_health_payload(payload)
        assert_true(any(key in decoded for key in ("t_x10", "temp_c", "temperature_c")), "sensor payload should decode")


def test_lab7_bridge_checks() -> None:
    info("unit: SCADA bridge mapping")
    output = run_python(SCADA_UNIT_PATH)
    assert_match(output, r"All bridge checks passed\.", "SCADA bridge unit test should pass")

    info("e2e: local bridge to dashboard structure")
    output = run_python(SCADA_E2E_PATH)
    assert_match(output, r"Thread -> bridge -> dashboard structure validated\.", "SCADA bridge E2E structure should pass")


def test_lab7_live_checks() -> None:
    info("live: OTBR state")
    manual_node_addr = os.environ.get("SOILSENSE_THREAD_NODE_ADDR")
    manual_node_rloc16 = os.environ.get("SOILSENSE_THREAD_RLOC16")

    try:
        state = bridge.run_otctl("state").strip()
        assert_true(state in {"leader", "router", "child"}, f"OTBR state unexpected: {state}")

        info("live: router discovery")
        remote_rloc16, ext_mac, avg_rssi, last_rssi = bridge.discover_remote_router()
        assert_true(remote_rloc16 > 0, "remote router RLOC16 should be discovered")
        info(f"remote router=0x{remote_rloc16:04x} ext_mac={ext_mac or 'n/a'} avg_rssi={avg_rssi} last_rssi={last_rssi}")

        info("live: health poll")
        telemetry = bridge.poll_once("sys/health")
        assert_true(telemetry.rloc16 == remote_rloc16, "telemetry should track the remote router RLOC16")

        info("live: router table consistency")
        router_table = bridge.run_otctl("router", "table")
        assert_match(
            router_table,
            rf"0x{remote_rloc16:04x}",
            "remote router should appear in router table",
        )
    except subprocess.CalledProcessError as exc:
        if not manual_node_addr:
            raise CheckError(
                "ot-ctl was not reachable. Run `sudo -v` first or set "
                "SOILSENSE_THREAD_NODE_ADDR to the node's IPv6 address and rerun."
            ) from exc

        info("auto-discovery unavailable; using provided node address")
        payload = bridge.coap_get(manual_node_addr, "sys/health")
        decoded = bridge.parse_health_payload(payload)
        fallback_rloc16 = int(manual_node_rloc16, 16) if manual_node_rloc16 else 0
        telemetry = bridge.cbor_to_telemetry(decoded, fallback_rloc16, "router", manual_node_addr)

    assert_true(telemetry.batt_mv >= 0, "battery should be readable")
    assert_true(telemetry.uptime_s >= 0, "uptime should be non-negative")
    assert_true(telemetry.role in {"router", "child", "leader"}, f"unexpected role: {telemetry.role}")

    info("live: router table consistency")
    if "remote_rloc16" in locals():
        assert_true(telemetry.rloc16 == remote_rloc16, "telemetry should track the remote router RLOC16")

    info("live: bridge output formatting")
    line = bridge.influx_line(telemetry)
    assert_match(line, r"^thread_battery,", "bridge measurement should exist")


def test_lab8_static_checks() -> None:
    info("static: release version stamps")
    root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    ftd_cmake = (ROOT / "ftd_battery" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert_match(root_cmake, r'set\(PROJECT_VER\s+"1\.0\.0"\)', "root project should be versioned")
    assert_match(ftd_cmake, r'set\(PROJECT_VER\s+"1\.0\.0"\)', "FTD project should be versioned")

    info("static: secrets are ignored")
    gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
    assert_match(gitignore, r"^main/secrets\.h$", "root secrets header should be ignored")
    assert_match(gitignore, r"^ftd_battery/main/secrets\.h$", "FTD secrets header should be ignored")

    tracked_secrets = run_cmd(["git", "ls-files", "*secrets.h"], cwd=ROOT).splitlines()
    assert_true(
        all(not path.endswith("/secrets.h") for path in tracked_secrets),
        f"no concrete secrets.h file should be tracked: {tracked_secrets}",
    )

    info("static: bridge defaults")
    bridge_source = BRIDGE_PATH.read_text(encoding="utf-8")
    assert_match(bridge_source, r'DEFAULT_HEALTH_URI\s*=\s*"sys/health"', "health URI should be stable")
    assert_match(bridge_source, r"thread_battery", "bridge should emit thread_battery measurement")

    info("static: secrets templates")
    root_secret_example = (ROOT / "main" / "secrets.h.example").read_text(encoding="utf-8")
    ftd_secret_example = (ROOT / "ftd_battery" / "main" / "secrets.h.example").read_text(encoding="utf-8")
    assert_match(root_secret_example, r'COAP_PSK_IDENTITY\s+"replace-me"', "root secret template should use placeholders")
    assert_match(ftd_secret_example, r'COAP_PSK_KEY\s+"replace-me"', "FTD secret template should use placeholders")

    info("static: child-role configuration")
    sensor_cli = (ROOT / "Sensores" / "main" / "esp_ot_cli.c").read_text(encoding="utf-8")
    water_cli = (ROOT / "Nivel_Agua" / "main" / "esp_ot_cli.c").read_text(encoding="utf-8")
    battery_src = (ROOT / "ftd_battery" / "main" / "ftd_battery.c").read_text(encoding="utf-8")
    assert_match(sensor_cli, r"otThreadSetRouterEligible\(instance,\s*false\)", "sensor node should force child attachment")
    assert_match(water_cli, r"otThreadSetRouterEligible\(instance,\s*false\)", "water node should force child attachment")
    assert_match(battery_src, r"otThreadBecomeChild", "battery node should request child role")
    assert_match(sensor_cli, r"OpenThread child \+ CoAP resource /env/temp", "sensor startup log should describe Thread/CoAP role")
    assert_match(water_cli, r"OpenThread child \+ CoAP resources /nivel and /act/valve", "water startup log should describe Thread/CoAP role")

    info("static: CoAP hardening hooks")
    sensor_valve = (ROOT / "Sensores" / "main" / "valve_demo.c").read_text(encoding="utf-8")
    water_valve = (ROOT / "Nivel_Agua" / "main" / "valve_demo.c").read_text(encoding="utf-8")
    assert_match(sensor_valve, r"COAP_RESPONSE_CODE_BAD_REQUEST", "sensor valve path should reject malformed payloads")
    assert_match(water_valve, r"COAP_RESPONSE_CODE_BAD_REQUEST", "water valve path should reject malformed payloads")


def print_manual_checklist(lab: str) -> None:
    print(f"Manual checklist for {lab}:")
    for item in MANUAL_CHECKLISTS[lab]:
        print(f" - {item}")


def test_lab5_manual_checklist() -> None:
    info("manual: Lab 5 required hardware checks")
    print_manual_checklist("lab5")


def test_lab7_manual_checklist() -> None:
    info("manual: Lab 7 required operator checks")
    print_manual_checklist("lab7")


def test_lab8_manual_checklist() -> None:
    info("manual: Lab 8 required chaos and hardware checks")
    print_manual_checklist("lab8")


SUITES: dict[str, List[Callable[[], None]]] = {
    "lab5": [test_bridge_unit_checks, test_lab5_static_checks, test_lab5_live_checks, test_lab5_manual_checklist],
    "lab7": [test_bridge_unit_checks, test_lab7_bridge_checks, test_lab7_live_checks, test_lab7_manual_checklist],
    "lab8": [test_bridge_unit_checks, test_lab8_static_checks, test_lab8_manual_checklist],
    "all": [
        test_bridge_unit_checks,
        test_lab5_static_checks,
        test_lab5_live_checks,
        test_lab5_manual_checklist,
        test_lab7_bridge_checks,
        test_lab7_live_checks,
        test_lab7_manual_checklist,
        test_lab8_static_checks,
        test_lab8_manual_checklist,
    ],
}


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description="Fast checks for Lab 5, Lab 7 and Lab 8.")
    parser.add_argument("suite", choices=sorted(SUITES.keys()))
    args = parser.parse_args(list(argv))

    checks = SUITES[args.suite]
    failures: List[str] = []
    for check in checks:
        try:
            check()
            info(f"PASS {check.__name__}")
        except Exception as exc:
            failures.append(f"{check.__name__}: {exc}")
            print(f"[check] FAIL {check.__name__}: {exc}", file=sys.stderr)

    if failures:
        print("\nSummary:")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("\nAll checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
