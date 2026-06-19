#!/usr/bin/env python3
"""Unit checks for the Thread -> SCADA bridge."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRIDGE_PATH = ROOT / "tools" / "thread_to_scada_bridge.py"
SPEC = importlib.util.spec_from_file_location("thread_to_scada_bridge", BRIDGE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Unable to load bridge module from {BRIDGE_PATH}")
bridge = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bridge
SPEC.loader.exec_module(bridge)


def encode_cbor_int(value: int) -> bytes:
    if value >= 0:
        if value < 24:
            return bytes([value])
        if value < 256:
            return bytes([24, value])
        if value < 65536:
            return bytes([25]) + value.to_bytes(2, "big")
        raise ValueError("positive integer too large for test encoder")
    n = -1 - value
    if n < 24:
        return bytes([0x20 | n])
    if n < 256:
        return bytes([0x20 | 24, n])
    if n < 65536:
        return bytes([0x20 | 25]) + n.to_bytes(2, "big")
    raise ValueError("negative integer too large for test encoder")


def encode_cbor_text(text: str) -> bytes:
    data = text.encode("utf-8")
    return bytes([0x60 | len(data)]) + data


def encode_cbor_map(entries: dict[str, int | str]) -> bytes:
    payload = bytes([0xA0 | len(entries)])
    for key, value in entries.items():
        payload += encode_cbor_text(key)
        payload += encode_cbor_text(value) if isinstance(value, str) else encode_cbor_int(value)
    return payload


def assert_true(condition: bool, msg: str) -> None:
    if not condition:
        raise AssertionError(msg)


def main() -> int:
    sensor_payload = encode_cbor_map(
        {"t_x10": 254, "h_x10": 612, "soil_x10": 734, "node": "nodo1"}
    )
    water_payload = encode_cbor_map(
        {"level": 88, "raw": 1, "pump": 0, "node": "nodo2"}
    )

    sensor = bridge.decode_sensor_payload(sensor_payload, "fd11::1")
    water = bridge.decode_water_payload(water_payload, "fd11::2")
    merged = bridge.build_dashboard_payload(sensor, water)

    assert_true(sensor.temperature_c == 25.4, "temperature should decode")
    assert_true(round(sensor.air_humidity_pct) == 61, "air humidity should decode")
    assert_true(round(sensor.soil_humidity_pct) == 73, "soil humidity should decode")
    assert_true(water.level_pct == 88, "water level should decode")
    assert_true(merged["hum_a"] == 61, "dashboard hum_a should match")
    assert_true(merged["hum_s"] == 73, "dashboard hum_s should match")
    assert_true(merged["level"] == 88, "dashboard level should match")
    assert_true("temp_c" in merged, "metadata should include temperature")
    assert_true(merged["sensor_node"] == "nodo1", "sensor node id should propagate")
    assert_true(merged["water_node"] == "nodo2", "water node id should propagate")

    alt_sensor_payload = encode_cbor_map(
        {"temp_c": 26, "air_humidity_pct": 104, "soil_humidity_pct": -5, "node": "alt1"}
    )
    alt_water_payload = encode_cbor_map(
        {"water_level_pct": 127, "pump": 2, "raw": 0, "node": "alt2"}
    )

    alt_sensor = bridge.decode_sensor_payload(alt_sensor_payload, "fd11::10")
    alt_water = bridge.decode_water_payload(alt_water_payload, "fd11::20")
    alt_merged = bridge.build_dashboard_payload(alt_sensor, alt_water, include_meta=False)

    assert_true(alt_sensor.temperature_c == 26.0, "alternate temp key should decode")
    assert_true(alt_sensor.air_humidity_pct == 100.0, "humidity should clamp to 100")
    assert_true(alt_sensor.soil_humidity_pct == 0.0, "soil humidity should clamp to 0")
    assert_true(alt_water.level_pct == 100, "water level should clamp to 100")
    assert_true(alt_water.pump_on == 1, "pump state should normalize to boolean int")
    assert_true(set(alt_merged.keys()) == {"hum_a", "hum_s", "level"}, "no-meta payload should stay minimal")

    print("All bridge checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
