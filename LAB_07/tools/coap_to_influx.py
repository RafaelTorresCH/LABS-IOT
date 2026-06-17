#!/usr/bin/env python3
"""
Poll the Thread battery node over CoAP and write its telemetry to InfluxDB.

This bridge is intentionally dependency-light:
- CoAP client is implemented with the Python standard library.
- CBOR decoding is implemented only for the small payload used by the lab.
- InfluxDB writes use the `requests` package, which is already available in
  this workspace.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shlex
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Tuple

import requests


COAP_PORT = 5683
COAP_TYPE_CON = 0
COAP_CODE_GET = 1
COAP_CODE_CONTENT = 69  # 2.05
COAP_OPT_URI_PATH = 11
COAP_OPT_ACCEPT = 17
COAP_CONTENT_FORMAT_CBOR = 60

DEFAULT_PREFIX = "fd11:22:33:44::/64"
DEFAULT_HEALTH_URI = "sys/health"
DEFAULT_INTERVAL_S = 30


@dataclass
class Telemetry:
    batt_mv: int
    rssi_dbm: int
    uptime_s: int
    rloc16: int
    role: str
    source_addr: str


def log(msg: str) -> None:
    print(f"[bridge] {msg}", flush=True)


def run_otctl(*args: str) -> str:
    cmd = shlex.split(os.environ.get("SOILSENSE_OTCTL", "sudo ot-ctl"))
    completed = subprocess.run(
        cmd + list(args),
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def parse_kv_line(output: str, prefix: str) -> Optional[str]:
    for line in output.splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :].strip()
    return None


def get_mesh_local_prefix() -> str:
    active = run_otctl("dataset", "active")
    prefix = parse_kv_line(active, "Mesh Local Prefix:")
    if not prefix:
        return DEFAULT_PREFIX
    return prefix


def get_local_rloc16() -> int:
    raw = run_otctl("rloc16")
    match = re.search(r"0x([0-9a-fA-F]{4})", raw)
    if not match:
        raise RuntimeError(f"Could not parse local RLOC16 from: {raw!r}")
    return int(match.group(1), 16)


def _split_table_row(line: str) -> List[str]:
    return [part.strip() for part in line.strip().strip("|").split("|")]


def _extract_rows(output: str) -> List[List[str]]:
    rows: List[List[str]] = []
    for line in output.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        if set(line) <= {"|", "+", "-", " "}:
            continue
        rows.append(_split_table_row(line))
    return rows


def discover_remote_router() -> Tuple[int, Optional[str], Optional[int], Optional[int]]:
    """
    Return (rloc16, ext_mac, avg_rssi, last_rssi) for the remote router.
    """

    local_rloc16 = get_local_rloc16()
    neighbor_rows = _extract_rows(run_otctl("neighbor", "table"))
    for row in neighbor_rows:
        if len(row) < 10:
            continue
        role = row[0]
        try:
            rloc16 = int(row[1], 16)
        except ValueError:
            continue
        if rloc16 == local_rloc16:
            continue
        if role.upper() != "R":
            continue

        avg_rssi = None
        last_rssi = None
        try:
            avg_rssi = int(row[3])
        except ValueError:
            pass
        try:
            last_rssi = int(row[4])
        except ValueError:
            pass
        ext_mac = row[9] if len(row) > 9 else None
        return rloc16, ext_mac, avg_rssi, last_rssi

    router_rows = _extract_rows(run_otctl("router", "table"))
    for row in router_rows:
        if len(row) < 8:
            continue
        try:
            rloc16 = int(row[1], 16)
        except ValueError:
            continue
        if rloc16 == local_rloc16:
            continue
        ext_mac = row[7] if len(row) > 7 else None
        return rloc16, ext_mac, None, None

    raise RuntimeError("No remote Thread router was found in ot-ctl tables")


def build_thread_addr(prefix: str, rloc16: int) -> str:
    base = prefix.split("/")[0].rstrip(":")
    return f"{base}:0:ff:fe00:{rloc16:04x}"


def _encode_option_nibble(value: int) -> Tuple[int, bytes]:
    if value < 13:
        return value, b""
    if value < 269:
        return 13, bytes([value - 13])
    if value < 65805:
        return 14, struct.pack("!H", value - 269)
    raise ValueError("CoAP option value too large")


def _encode_option(delta: int, value: bytes) -> bytes:
    delta_nibble, delta_ext = _encode_option_nibble(delta)
    length_nibble, length_ext = _encode_option_nibble(len(value))
    first = (delta_nibble << 4) | length_nibble
    return bytes([first]) + delta_ext + length_ext + value


def coap_get(uri_host: str, uri_path: str, timeout_s: float = 5.0) -> bytes:
    token = random.randbytes(4) if hasattr(random, "randbytes") else os.urandom(4)
    msg_id = random.randint(0, 0xFFFF)

    header = bytes(
        [
            (1 << 6) | (COAP_TYPE_CON << 4) | len(token),
            COAP_CODE_GET,
            (msg_id >> 8) & 0xFF,
            msg_id & 0xFF,
        ]
    ) + token

    options = b""
    last_option = 0
    for path_part in [part for part in uri_path.split("/") if part]:
        option_value = path_part.encode("utf-8")
        options += _encode_option(COAP_OPT_URI_PATH - last_option, option_value)
        last_option = COAP_OPT_URI_PATH

    options += _encode_option(COAP_OPT_ACCEPT - last_option, bytes([COAP_CONTENT_FORMAT_CBOR]))
    packet = header + options

    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        sock.sendto(packet, (uri_host, COAP_PORT, 0, 0))
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

            payload_index = 4 + resp_tkl
            while payload_index < len(data):
                byte = data[payload_index]
                if byte == 0xFF:
                    return data[payload_index + 1 :]
                payload_index += 1
            if resp_code == COAP_CODE_CONTENT:
                return b""


def _decode_length(buf: bytes, idx: int, addl: int) -> Tuple[int, int]:
    if addl < 24:
        return addl, idx
    if addl == 24:
        return buf[idx], idx + 1
    if addl == 25:
        return struct.unpack_from("!H", buf, idx)[0], idx + 2
    if addl == 26:
        return struct.unpack_from("!I", buf, idx)[0], idx + 4
    raise ValueError("Unsupported CBOR length encoding")


def decode_cbor(buf: bytes, idx: int = 0) -> Tuple[Any, int]:
    if idx >= len(buf):
        raise ValueError("CBOR buffer ended unexpectedly")

    initial = buf[idx]
    idx += 1
    major = initial >> 5
    addl = initial & 0x1F
    value, idx = _decode_length(buf, idx, addl)

    if major == 0:
        return value, idx
    if major == 1:
        return -1 - value, idx
    if major == 2:
        return buf[idx : idx + value], idx + value
    if major == 3:
        return buf[idx : idx + value].decode("utf-8"), idx + value
    if major == 4:
        items = []
        for _ in range(value):
            item, idx = decode_cbor(buf, idx)
            items.append(item)
        return items, idx
    if major == 5:
        mapping: Dict[Any, Any] = {}
        for _ in range(value):
            key, idx = decode_cbor(buf, idx)
            val, idx = decode_cbor(buf, idx)
            mapping[key] = val
        return mapping, idx
    if major == 7:
        if addl == 20:
            return False, idx
        if addl == 21:
            return True, idx
        if addl == 22:
            return None, idx
        return value, idx

    raise ValueError(f"Unsupported CBOR major type: {major}")


def parse_health_payload(payload: bytes) -> Dict[str, Any]:
    decoded, idx = decode_cbor(payload)
    if idx != len(payload):
        raise ValueError("Trailing bytes found in CBOR payload")
    if not isinstance(decoded, dict):
        raise ValueError("Expected CBOR map in health payload")
    return decoded


def cbor_to_telemetry(payload: Dict[str, Any], fallback_rloc16: int, fallback_role: str, source_addr: str) -> Telemetry:
    batt_mv = int(payload.get("batt", 0))
    rssi_dbm = int(payload.get("rssi", -127))
    uptime_s = int(payload.get("up", 0))
    rloc16 = int(payload.get("rloc16", fallback_rloc16))
    role = str(payload.get("role", fallback_role))
    return Telemetry(
        batt_mv=batt_mv,
        rssi_dbm=rssi_dbm,
        uptime_s=uptime_s,
        rloc16=rloc16,
        role=role,
        source_addr=source_addr,
    )


def escape_line_value(value: str) -> str:
    return value.replace("\\", "\\\\").replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")


def influx_line(telemetry: Telemetry) -> str:
    tags = [
        f"node={escape_line_value(f'0x{telemetry.rloc16:04x}')}",
        f"role={escape_line_value(telemetry.role)}",
        f"addr={escape_line_value(telemetry.source_addr)}",
    ]
    fields = [
        f"batt_mv={telemetry.batt_mv}i",
        f"rssi_dbm={telemetry.rssi_dbm}i",
        f"uptime_s={telemetry.uptime_s}i",
    ]
    return "thread_battery," + ",".join(tags) + " " + ",".join(fields)


def write_influx(line: str) -> None:
    influx_url = os.environ.get("SOILSENSE_INFLUX_URL", "http://127.0.0.1:8086")
    org = os.environ.get("SOILSENSE_INFLUX_ORG", "")
    bucket = os.environ.get("SOILSENSE_INFLUX_BUCKET", "")
    token = os.environ.get("SOILSENSE_INFLUX_TOKEN", "")
    if not org or not bucket:
        log(f"InfluxDB not configured, sample: {line}")
        return

    url = influx_url.rstrip("/") + "/api/v2/write"
    params = {"org": org, "bucket": bucket, "precision": "ns"}
    headers = {"Content-Type": "text/plain; charset=utf-8"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    response = requests.post(url, params=params, headers=headers, data=line.encode("utf-8"), timeout=10)
    response.raise_for_status()


def poll_once(uri_path: str) -> Telemetry:
    prefix = get_mesh_local_prefix()
    remote_rloc16, remote_ext_mac, avg_rssi, last_rssi = discover_remote_router()
    target_addr = build_thread_addr(prefix, remote_rloc16)
    log(f"Polling {target_addr} (rloc16=0x{remote_rloc16:04x}, extmac={remote_ext_mac or 'n/a'})")
    payload = coap_get(target_addr, uri_path)
    decoded = parse_health_payload(payload)
    fallback_rssi = last_rssi if last_rssi is not None else (avg_rssi if avg_rssi is not None else -127)
    telemetry = cbor_to_telemetry(decoded, remote_rloc16, "router", target_addr)
    if telemetry.rssi_dbm == -127 and fallback_rssi != -127:
        telemetry.rssi_dbm = fallback_rssi
    return telemetry


def main() -> int:
    parser = argparse.ArgumentParser(description="Bridge Thread battery health to InfluxDB.")
    parser.add_argument("--once", action="store_true", help="Run one sample and exit.")
    parser.add_argument("--interval", type=int, default=DEFAULT_INTERVAL_S, help="Polling interval in seconds.")
    parser.add_argument("--uri-path", default=DEFAULT_HEALTH_URI, help="CoAP resource path.")
    args = parser.parse_args()

    while True:
        try:
            telemetry = poll_once(args.uri_path)
            line = influx_line(telemetry)
            write_influx(line)
            log(
                f"battery={telemetry.batt_mv}mV rssi={telemetry.rssi_dbm}dBm "
                f"up={telemetry.uptime_s}s node=0x{telemetry.rloc16:04x} role={telemetry.role}"
            )
        except subprocess.CalledProcessError as exc:
            log(f"ot-ctl failed: {exc.stderr.strip() if exc.stderr else exc}")
        except (socket.timeout, TimeoutError):
            log("CoAP request timed out")
        except Exception as exc:
            log(f"bridge error: {exc}")

        if args.once:
            break
        time.sleep(args.interval)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
