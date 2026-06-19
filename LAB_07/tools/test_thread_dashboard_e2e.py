#!/usr/bin/env python3
"""End-to-end local validation of the Thread -> bridge -> dashboard structure.

This test does not require real Thread hardware. It validates the exact
structure used in the project:

1. The bridge builds a dashboard payload from simulated sensor snapshots.
2. The bridge sends that payload as CoAP POST JSON to `/sensores`.
3. A minimal local CoAP dashboard stub receives the JSON and replies 2.04.
"""

from __future__ import annotations

import importlib.util
import json
import socket
import sys
import threading
from pathlib import Path
from typing import Any, Dict


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_PATH = ROOT / "tools" / "thread_to_scada_bridge.py"
SPEC = importlib.util.spec_from_file_location("thread_to_scada_bridge", BRIDGE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Unable to load bridge module from {BRIDGE_PATH}")
bridge = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bridge
SPEC.loader.exec_module(bridge)


def assert_true(condition: bool, msg: str) -> None:
    if not condition:
        raise AssertionError(msg)


def _decode_option_value(packet: bytes, idx: int, nibble: int) -> tuple[int, int]:
    if nibble < 13:
        return nibble, idx
    if nibble == 13:
        return packet[idx] + 13, idx + 1
    if nibble == 14:
        return int.from_bytes(packet[idx : idx + 2], "big") + 269, idx + 2
    raise ValueError("Unsupported CoAP extended option")


def decode_coap_json(packet: bytes) -> Dict[str, Any]:
    assert_true(len(packet) >= 4, "packet too short")
    token_len = packet[0] & 0x0F
    idx = 4 + token_len
    last_option = 0
    path_parts = []

    while idx < len(packet):
        if packet[idx] == 0xFF:
            idx += 1
            break

        first = packet[idx]
        idx += 1
        delta_nibble = (first >> 4) & 0x0F
        length_nibble = first & 0x0F
        delta, idx = _decode_option_value(packet, idx, delta_nibble)
        length, idx = _decode_option_value(packet, idx, length_nibble)
        option_number = last_option + delta
        option_value = packet[idx : idx + length]
        idx += length
        last_option = option_number

        if option_number == 11:
            path_parts.append(option_value.decode("utf-8"))

    path = "/".join(path_parts)
    payload = packet[idx:].decode("utf-8")
    parsed = json.loads(payload)
    parsed["_path"] = path
    return parsed


def build_changed_response(request: bytes) -> bytes:
    token_len = request[0] & 0x0F
    msg_id = request[2:4]
    token = request[4 : 4 + token_len]
    version = (request[0] >> 6) & 0x03
    response_type = 2  # ACK
    return bytes([(version << 6) | (response_type << 4) | token_len, 68]) + msg_id + token


def main() -> int:
    captured: dict[str, Any] = {}
    ready = threading.Event()

    def dashboard_stub() -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            captured["port"] = sock.getsockname()[1]
            ready.set()
            packet, addr = sock.recvfrom(4096)
            captured["payload"] = decode_coap_json(packet)
            sock.sendto(build_changed_response(packet), addr)

    server_thread = threading.Thread(target=dashboard_stub, daemon=True)
    server_thread.start()
    ready.wait(timeout=2.0)
    assert_true("port" in captured, "dashboard stub did not start")

    sensor = bridge.build_simulated_sensor("fd11::7001")
    water = bridge.build_simulated_water("fd11::7002", 52)
    payload = bridge.build_dashboard_payload(sensor, water, include_meta=True)
    bridge.coap_post_json("127.0.0.1", "sensores", payload, timeout_s=2.0, port=int(captured["port"]))

    server_thread.join(timeout=2.0)
    assert_true("payload" in captured, "dashboard did not receive a payload")

    received = captured["payload"]
    assert_true(received["_path"] == "sensores", "dashboard path should be sensores")
    assert_true(received["hum_a"] == 63, "hum_a should match bridge payload")
    assert_true(received["hum_s"] == 71, "hum_s should match bridge payload")
    assert_true(received["level"] == 52, "level should match bridge payload")
    assert_true(received["temp_c"] == 24.8, "temp_c metadata should be preserved")

    print("Thread -> bridge -> dashboard structure validated.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
