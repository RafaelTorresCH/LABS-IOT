#!/usr/bin/env python3
"""
Poll Thread CoAP nodes and forward dashboard-compatible JSON to sample_project-main.

This bridge does not modify sample_project-main. It simply sends the payload
that project already expects at POST coap://<dashboard-host>/sensores:

    {"hum_a": <int>, "hum_s": <int>, "level": <int>}

Optional extras are also added for observability, but the sample dashboard
ignores unknown keys safely.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import random
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
COMMON_PATH = ROOT / "tools" / "coap_to_influx.py"
COMMON_SPEC = importlib.util.spec_from_file_location("coap_to_influx", COMMON_PATH)
if COMMON_SPEC is None or COMMON_SPEC.loader is None:
    raise RuntimeError(f"Unable to load common CoAP helpers from {COMMON_PATH}")
common = importlib.util.module_from_spec(COMMON_SPEC)
sys.modules[COMMON_SPEC.name] = common
COMMON_SPEC.loader.exec_module(common)

COAP_TYPE_CON = 0
COAP_CODE_POST = 2
COAP_CODE_CHANGED = 68  # 2.04
COAP_OPT_URI_PATH = 11
COAP_OPT_CONTENT_FORMAT = 12
COAP_CONTENT_FORMAT_JSON = 50
DEFAULT_DASHBOARD_PORT = common.COAP_PORT
DEFAULT_INTERVAL_S = 5


@dataclass
class SensorSnapshot:
    temperature_c: float
    air_humidity_pct: float
    soil_humidity_pct: float
    source_addr: str
    node_id: str = "sensor"


@dataclass
class WaterSnapshot:
    level_pct: int
    pump_on: int
    raw_state: int
    source_addr: str
    node_id: str = "water"


def log(msg: str) -> None:
    print(f"[scada-bridge] {msg}", flush=True)


def _encode_option_nibble(value: int):
    if value < 13:
        return value, b""
    if value < 269:
        return 13, bytes([value - 13])
    if value < 65805:
        return 14, value.to_bytes(2, "big")
    raise ValueError("CoAP option value too large")


def _encode_option(delta: int, value: bytes) -> bytes:
    delta_nibble, delta_ext = _encode_option_nibble(delta)
    length_nibble, length_ext = _encode_option_nibble(len(value))
    first = (delta_nibble << 4) | length_nibble
    return bytes([first]) + delta_ext + length_ext + value


def coap_post_json(host: str, uri_path: str, payload: Dict[str, Any], timeout_s: float = 5.0, port: int = DEFAULT_DASHBOARD_PORT) -> None:
    token = random.randbytes(4) if hasattr(random, "randbytes") else os.urandom(4)
    msg_id = random.randint(0, 0xFFFF)
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")

    header = bytes(
        [
            (1 << 6) | (COAP_TYPE_CON << 4) | len(token),
            COAP_CODE_POST,
            (msg_id >> 8) & 0xFF,
            msg_id & 0xFF,
        ]
    ) + token

    options = b""
    last_option = 0
    for path_part in [part for part in uri_path.split("/") if part]:
        options += _encode_option(COAP_OPT_URI_PATH - last_option, path_part.encode("utf-8"))
        last_option = COAP_OPT_URI_PATH

    options += _encode_option(COAP_OPT_CONTENT_FORMAT - last_option, bytes([COAP_CONTENT_FORMAT_JSON]))
    packet = header + options + b"\xFF" + body

    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    target = (host, port, 0, 0) if family == socket.AF_INET6 else (host, port)

    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        sock.sendto(packet, target)
        while True:
            data, _addr = sock.recvfrom(2048)
            if len(data) < 4:
                continue
            resp_tkl = data[0] & 0x0F
            resp_code = data[1]
            resp_msg_id = (data[2] << 8) | data[3]
            if resp_msg_id != msg_id:
                continue
            if data[4 : 4 + resp_tkl] != token:
                continue
            if resp_code != COAP_CODE_CHANGED:
                raise RuntimeError(f"Dashboard CoAP response unexpected: {resp_code}")
            return


def clamp_int(value: int, lower: int, upper: int) -> int:
    return max(lower, min(upper, value))


def _scaled_tenth(decoded: Dict[str, Any], primary_key: str, fallback_keys: Tuple[str, ...] = ()) -> float:
    if primary_key in decoded:
        return float(int(decoded[primary_key]) / 10.0)
    for key in fallback_keys:
        if key in decoded:
            return float(decoded[key])
    raise KeyError(f"Missing telemetry field: {primary_key}")


def _safe_node_id(decoded: Dict[str, Any], default: str) -> str:
    value = decoded.get("node", default)
    text = str(value).strip()
    return text or default


def _sanitize_sensor_snapshot(sensor: SensorSnapshot) -> SensorSnapshot:
    return SensorSnapshot(
        temperature_c=round(sensor.temperature_c, 1),
        air_humidity_pct=float(clamp_int(int(round(sensor.air_humidity_pct)), 0, 100)),
        soil_humidity_pct=float(clamp_int(int(round(sensor.soil_humidity_pct)), 0, 100)),
        source_addr=sensor.source_addr,
        node_id=sensor.node_id,
    )


def _sanitize_water_snapshot(water: WaterSnapshot) -> WaterSnapshot:
    return WaterSnapshot(
        level_pct=clamp_int(int(round(water.level_pct)), 0, 100),
        pump_on=1 if int(water.pump_on) else 0,
        raw_state=1 if int(water.raw_state) else 0,
        source_addr=water.source_addr,
        node_id=water.node_id,
    )


def _addr_from_env(name_addr: str, name_rloc16: str) -> Optional[str]:
    addr = os.environ.get(name_addr)
    if addr:
        return addr
    rloc16 = os.environ.get(name_rloc16)
    if not rloc16:
        return None
    return common.build_thread_addr(common.get_mesh_local_prefix(), int(rloc16, 16))


def decode_sensor_payload(payload: bytes, source_addr: str) -> SensorSnapshot:
    decoded = common.parse_health_payload(payload)
    snapshot = SensorSnapshot(
        temperature_c=_scaled_tenth(decoded, "t_x10", ("temp_c", "temperature_c")),
        air_humidity_pct=_scaled_tenth(decoded, "h_x10", ("hum_a", "air_humidity_pct")),
        soil_humidity_pct=_scaled_tenth(decoded, "soil_x10", ("hum_s", "soil_humidity_pct")),
        source_addr=source_addr,
        node_id=_safe_node_id(decoded, "sensor"),
    )
    return _sanitize_sensor_snapshot(snapshot)


def decode_water_payload(payload: bytes, source_addr: str) -> WaterSnapshot:
    decoded = common.parse_health_payload(payload)
    level = decoded.get("level")
    if level is None and "level_x10" in decoded:
        level = int(decoded["level_x10"]) / 10.0
    if level is None and "water_level_pct" in decoded:
        level = decoded["water_level_pct"]
    snapshot = WaterSnapshot(
        level_pct=int(round(float(level or 0))),
        pump_on=int(decoded.get("pump", 0)),
        raw_state=int(decoded.get("raw", 0)),
        source_addr=source_addr,
        node_id=_safe_node_id(decoded, "water"),
    )
    return _sanitize_water_snapshot(snapshot)


def build_dashboard_payload(sensor: SensorSnapshot, water: WaterSnapshot, include_meta: bool = True) -> Dict[str, Any]:
    payload: Dict[str, Any] = {
        "hum_a": int(round(sensor.air_humidity_pct)),
        "hum_s": int(round(sensor.soil_humidity_pct)),
        "level": int(water.level_pct),
    }
    if include_meta:
        payload.update(
            {
                "temp_c": round(sensor.temperature_c, 1),
                "pump": int(water.pump_on),
                "sensor_addr": sensor.source_addr,
                "water_addr": water.source_addr,
                "sensor_node": sensor.node_id,
                "water_node": water.node_id,
                "ts_unix": int(time.time()),
            }
        )
    return payload


def poll_sensor(addr: str) -> SensorSnapshot:
    payload = common.coap_get(addr, "env/temp")
    return decode_sensor_payload(payload, addr)


def poll_water(addr: str) -> WaterSnapshot:
    payload = common.coap_get(addr, "nivel")
    return decode_water_payload(payload, addr)


def build_simulated_sensor(addr: str) -> SensorSnapshot:
    return SensorSnapshot(
        temperature_c=24.8,
        air_humidity_pct=63.0,
        soil_humidity_pct=71.0,
        source_addr=addr,
        node_id="sensor-sim",
    )


def build_simulated_water(addr: str, fallback_level: int) -> WaterSnapshot:
    return WaterSnapshot(
        level_pct=clamp_int(fallback_level, 0, 100),
        pump_on=0,
        raw_state=0,
        source_addr=addr,
        node_id="water-sim",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Bridge Thread CoAP telemetry to the SCADA dashboard CoAP API.")
    parser.add_argument("--dashboard-host", default=os.environ.get("SOILSENSE_SCADA_HOST", "127.0.0.1"))
    parser.add_argument("--dashboard-path", default=os.environ.get("SOILSENSE_SCADA_PATH", "sensores"))
    parser.add_argument("--dashboard-port", type=int, default=int(os.environ.get("SOILSENSE_SCADA_PORT", str(DEFAULT_DASHBOARD_PORT))))
    parser.add_argument("--sensor-addr", default=_addr_from_env("SOILSENSE_SENSOR_ADDR", "SOILSENSE_SENSOR_RLOC16"))
    parser.add_argument("--water-addr", default=_addr_from_env("SOILSENSE_WATER_ADDR", "SOILSENSE_WATER_RLOC16"))
    parser.add_argument("--level-fallback", type=int, default=int(os.environ.get("SOILSENSE_LEVEL_FALLBACK", "50")))
    parser.add_argument("--dry-run", action="store_true", help="Build payloads but do not send to the dashboard.")
    parser.add_argument("--simulate", action="store_true", help="Use simulated sensor values instead of polling Thread nodes.")
    parser.add_argument("--no-meta", action="store_true", help="Send only the three dashboard-required keys.")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--interval", type=int, default=int(os.environ.get("SOILSENSE_SCADA_INTERVAL_S", str(DEFAULT_INTERVAL_S))))
    args = parser.parse_args()

    if not args.sensor_addr and not args.simulate:
        raise SystemExit("Missing sensor address. Set --sensor-addr or SOILSENSE_SENSOR_ADDR/SOILSENSE_SENSOR_RLOC16.")

    sensor_addr = args.sensor_addr or "fd00::sensor-sim"
    water_addr = args.water_addr or "fd00::water-fallback"
    include_meta = not args.no_meta
    last_water = WaterSnapshot(level_pct=clamp_int(args.level_fallback, 0, 100), pump_on=0, raw_state=0, source_addr="fallback")

    while True:
        try:
            if args.simulate:
                sensor = build_simulated_sensor(sensor_addr)
                last_water = build_simulated_water(water_addr, args.level_fallback)
            else:
                sensor = poll_sensor(sensor_addr)
                if args.water_addr:
                    try:
                        last_water = poll_water(water_addr)
                    except Exception as exc:
                        log(f"water node unavailable, using last/fallback level={last_water.level_pct}: {exc}")

            payload = build_dashboard_payload(sensor, last_water, include_meta=include_meta)
            if args.dry_run:
                log(f"dry-run payload {json.dumps(payload, ensure_ascii=True)}")
            else:
                coap_post_json(args.dashboard_host, args.dashboard_path, payload, port=args.dashboard_port)
                log(
                    f"sent {json.dumps(payload, ensure_ascii=True)} "
                    f"-> coap://{args.dashboard_host}:{args.dashboard_port}/{args.dashboard_path}"
                )
        except Exception as exc:
            log(f"bridge error: {exc}")

        if args.once:
            break
        time.sleep(args.interval)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
