#!/usr/bin/env python3
"""BUILD-IOT-06 host check for the gateway MQTT diagnostic vector.

* Verifies that gateway/GatewayDiagnostics.h embeds the official Frame v1
  vector byte-for-byte and its public test key, and that the HMAC-SHA256 tag
  verifies (so the firmware self-check at boot must pass).
* Prints the exact JSON the diagnostic produces through the production
  builder (lora_rx_at=null, clock_synced=false, rssi=-87, snr=7.50) and the
  SHA-256 the backend stores as IotMqttDelivery.payload_sha256, so a staging
  operator can correlate serial output with the durable receipt.

  python tools/test_gateway_diag_vector.py [--uid GW-XXXXXXXXXX]
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "gateway" / "GatewayDiagnostics.h"
VECTOR = ROOT / "test_vectors" / "official_vector.json"

failures = 0


def check(name: str, ok: bool) -> None:
    global failures
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        failures += 1


def c_array(source: str, name: str) -> bytes:
    match = re.search(rf"{name}\[[^\]]*\]\s*=\s*\{{([^}}]*)\}}", source)
    if not match:
        raise SystemExit(f"array {name} not found in {HEADER}")
    return bytes(int(tok, 16) for tok in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))


def c_constant(source: str, pattern: str) -> str:
    match = re.search(pattern, source)
    if not match:
        raise SystemExit(f"constant {pattern!r} not found in {HEADER}")
    return match.group(1)


def diag_payload(frame: bytes, rssi: int, snr: float, buffered: bool) -> str:
    # Mirrors shared/src/AgrotechMqttPayload.cpp with clock_synced=false.
    return (
        '{"v":1,"frame":"%s","lora_rx_at":null,"clock_synced":false,'
        '"buffered":%s,"rssi":%d,"snr":%.2f}'
        % (base64.b64encode(frame).decode("ascii"), "true" if buffered else "false",
           rssi, snr)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uid", default="GW-7K3M9QXT2A",
                        help="staging gateway UID (only used to print the topic)")
    args = parser.parse_args()

    vector = json.loads(VECTOR.read_text(encoding="utf-8"))
    source = HEADER.read_text(encoding="utf-8")
    frame = c_array(source, "kFrame")
    key = c_array(source, "kPublicTestKey")
    rssi = int(c_constant(source, r"kRssi\s*=\s*(-?\d+)"))
    snr = float(c_constant(source, r"kSnr\s*=\s*(-?[\d.]+)f"))
    length = int(c_constant(source, r"kFrameLength\s*=\s*(\d+)"))

    expected = bytes.fromhex(vector["frame_hex"])
    check("diagnostic is disabled by default", "#define GATEWAY_MQTT_DIAG_ENABLED 0" in source)
    check("embedded frame equals official vector frame_hex", frame == expected)
    check("kFrameLength matches vector frame_length", length == len(frame) == vector["frame_length"])
    check("embedded key equals the public test key", key == bytes.fromhex(vector["key_hex"]))
    check("frame base64 equals vector frame_base64",
          base64.b64encode(frame).decode() == vector["frame_base64"])
    check("frame version byte is 1", frame[0] == vector["frame_version"])

    signed, tag = frame[:-32], frame[-32:]
    check("signed part equals vector signed_hex", signed.hex() == vector["signed_hex"])
    context = vector["hmac_context"].encode("ascii")
    calc = hmac.new(key, context + signed, hashlib.sha256).digest()
    check("HMAC-SHA256 tag verifies with the public test key",
          hmac.compare_digest(calc, tag) and tag.hex() == vector["tag_hex"])
    check("reference RSSI/SNR match the backend cross-check", rssi == -87 and abs(snr - 7.5) < 1e-9)

    topic = f"agrotech/v1/gw/{args.uid}/up"
    print(f"\ntopic: {topic}")
    for flag in (False, True):
        payload = diag_payload(frame, rssi, snr, flag)
        check(f"payload buffered={str(flag).lower()} fits MAX_MESSAGE_BYTES=444",
              len(payload.encode()) <= 444)
        print(f"  buffered={str(flag).lower():5} bytes={len(payload)} "
              f"payload_sha256={hashlib.sha256(payload.encode()).hexdigest()}")
        print(f"  {payload}")

    print(f"\n{failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
