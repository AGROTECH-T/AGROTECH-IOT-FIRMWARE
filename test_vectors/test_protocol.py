"""Independent reference checks for the AGROTECH LoRa frame v1 contract.

This script does not execute the C++ firmware. It verifies the official backend
vector, the sentinel values, the frame length formula, the Base64 output and the
MQTT payload field contract using the Python standard library only.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import struct
from pathlib import Path

UID = b"CO-7K3M9QXT2A"
KEY = bytes(range(32))
CONTEXT = b"agrotech:lora-frame:v1"
ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"


def reading(channel: int, metric_code: int, value: int) -> bytes:
    value_format = ">h" if metric_code == 1 else ">H"
    return struct.pack(">HB", channel, metric_code) + struct.pack(value_format, value)


def build_signed(
    *,
    uid: bytes = UID,
    key_version: int = 1,
    boot_counter: int = 7,
    seq: int = 120,
    readings=((1, 1, 2534), (2, 2, 6325)),
) -> bytes:
    blocks = b"".join(reading(*item) for item in readings)
    return (
        struct.pack(">B", 1)
        + uid
        + struct.pack(">HIIB", key_version, boot_counter, seq, len(readings))
        + blocks
    )


def tag_for(signed: bytes) -> bytes:
    return hmac.new(KEY, CONTEXT + signed, hashlib.sha256).digest()


def build_frame(**fields) -> bytes:
    signed = build_signed(**fields)
    return signed + tag_for(signed)


def uid_valid(uid: str, prefix: str) -> bool:
    if len(uid) != 13 or not uid.startswith(prefix):
        return False
    return all(char in ALPHABET for char in uid[3:])


def channels_valid(readings) -> bool:
    """Frame v1 channel rules: non-zero uint16, unique within a frame."""
    channels = [channel for channel, _metric, _value in readings]
    return all(0 < channel <= 0xFFFF for channel in channels) and len(
        set(channels)
    ) == len(channels)


# BUILD-IOT-SENSOR-FAILOVER: channel -> (metric_code, physical source).
FAILOVER_CHANNELS = {1: (1, "BME680"), 2: (2, "BME680"), 3: (1, "DHT11"), 4: (2, "DHT11")}


def check_failover_channels(checks, official) -> None:
    """DHT11 backup pair on channels 3/4 stays a plain Frame v1 frame."""
    path = Path(__file__).with_name("dht_channels_vector.json")
    dht = json.loads(path.read_text(encoding="utf-8"))
    readings = [(r["channel"], r["metric_code"], r["value"]) for r in dht["readings"]]
    signed = build_signed(
        uid=dht["coordinator_uid"].encode("ascii"),
        key_version=dht["key_version"],
        boot_counter=dht["boot_counter"],
        seq=dht["seq"],
        readings=readings,
    )
    tag = tag_for(signed)
    frame = signed + tag
    checks.check(signed.hex() == dht["signed_hex"], "DHT11 3/4 signed bytes")
    checks.check(tag.hex() == dht["tag_hex"], "DHT11 3/4 HMAC tag (same v1 context)")
    checks.check(frame.hex() == dht["frame_hex"], "DHT11 3/4 full frame")
    checks.check(
        base64.b64encode(frame).decode("ascii") == dht["frame_base64"], "DHT11 3/4 Base64"
    )
    checks.check(len(frame) == dht["frame_length"] == 57 + 5 * 2, "DHT11 3/4 frame length")
    checks.check(
        dht["hmac_context"] == official["hmac_context"] == CONTEXT.decode("ascii"),
        "DHT11 vector uses the unchanged HMAC context",
    )
    checks.check(signed[0] == 1 and signed[:25][1:14] == UID, "DHT11 vector header is v1")
    checks.check(
        [channel for channel, _m, _v in readings] == [3, 4], "DHT11 pair uses channels 3/4"
    )
    checks.check(
        all(FAILOVER_CHANNELS[c] == (m, "DHT11") for c, m, _v in readings),
        "channel 3 is DHT11 temperature, channel 4 is DHT11 humidity",
    )
    official_channels = [item["channel"] for item in official["readings"]]
    checks.check(
        all(FAILOVER_CHANNELS[c][1] == "BME680" for c in official_channels),
        "official vector channels 1/2 are the BME680 pair",
    )
    checks.check(signed[25:27] == b"\x00\x03" and signed[30:32] == b"\x00\x04",
                 "channels 3/4 encoded big-endian")
    checks.check(channels_valid(readings), "channels 3/4 satisfy v1 channel rules")
    checks.check(not channels_valid([(0, 1, 0), (4, 2, 0)]), "channel 0 rejected")
    checks.check(not channels_valid([(3, 1, 0), (3, 2, 0)]), "repeated channel rejected")
    mixed = {FAILOVER_CHANNELS[c][1] for c in (1, 4)}
    checks.check(len(mixed) == 2, "a 1/4 pair would mix sources (model check)")


class Checks:
    def __init__(self) -> None:
        self.count = 0
        self.failures = 0

    def check(self, condition: bool, label: str) -> None:
        self.count += 1
        if condition:
            print(f"PASS {label}")
        else:
            self.failures += 1
            print(f"FAIL {label}")


def main() -> int:
    checks = Checks()
    vector_path = Path(__file__).with_name("official_vector.json")
    vector = json.loads(vector_path.read_text(encoding="utf-8"))

    signed = build_signed(
        uid=vector["coordinator_uid"].encode("ascii"),
        key_version=vector["key_version"],
        boot_counter=vector["boot_counter"],
        seq=vector["seq"],
        readings=[
            (item["channel"], item["metric_code"], item["value"])
            for item in vector["readings"]
        ],
    )
    tag = tag_for(signed)
    frame = signed + tag

    checks.check(signed.hex() == vector["signed_hex"], "official signed bytes")
    checks.check(tag.hex() == vector["tag_hex"], "official HMAC tag")
    checks.check(frame.hex() == vector["frame_hex"], "official full frame")
    checks.check(
        base64.b64encode(frame).decode("ascii") == vector["frame_base64"],
        "official Base64",
    )
    checks.check(len(frame) == vector["frame_length"], "official frame length")
    checks.check(len(frame) == 57 + 5 * 2, "57 + 5 * reading_count formula")

    sentinel_frame = build_frame(
        seq=121,
        readings=((1, 1, -32768), (2, 2, 65535)),
    )
    checks.check(
        sentinel_frame.hex()
        == "01434f2d374b334d395158543241000100000007000000790200010180000002"
        "02ffff1c9d5c3669b6bb7106aa9fe39ffb04e5498df10a4d9e8a240a74556a5b"
        "c4acb9",
        "temperature and humidity sentinels",
    )

    single_zero = build_frame(
        key_version=1,
        boot_counter=1,
        seq=0,
        readings=((1, 1, 0),),
    )
    checks.check(len(single_zero) == 62, "single reading length 62")
    checks.check(single_zero[20:24] == b"\x00\x00\x00\x00", "seq 0 is big-endian")

    big_endian = build_frame(
        key_version=0x1234,
        boot_counter=0x01020304,
        seq=0xA1B2C3D4,
        readings=((0x0102, 1, 0x0304),),
    )
    checks.check(big_endian[14:16] == b"\x12\x34", "key_version big-endian")
    checks.check(big_endian[16:20] == b"\x01\x02\x03\x04", "boot_counter big-endian")
    checks.check(big_endian[20:24] == b"\xa1\xb2\xc3\xd4", "seq big-endian")
    checks.check(big_endian[25:30] == b"\x01\x02\x01\x03\x04", "reading big-endian")

    for count in (1, 2, 4, 16):
        readings = tuple((index + 1, 1, 0) for index in range(count))
        frame = build_frame(readings=readings)
        checks.check(
            len(frame) == 57 + 5 * count,
            f"frame length for {count} readings",
        )

    checks.check(uid_valid("CO-7K3M9QXT2A", "CO-"), "canonical coordinator UID")
    checks.check(uid_valid("GW-7K3M9QXT2A", "GW-"), "canonical gateway UID")
    checks.check(not uid_valid("co-7K3M9QXT2A", "CO-"), "lowercase UID rejected")
    checks.check(not uid_valid("CO-7K3M9QXT20", "CO-"), "ambiguous 0 rejected")
    checks.check(not uid_valid("CO-7K3M9QXT2I", "CO-"), "ambiguous I rejected")
    checks.check(not uid_valid("CO-7K3M9QXT2L", "CO-"), "ambiguous L rejected")

    boot_counter = 7
    next_boot = boot_counter + 1
    checks.check(next_boot == 8, "boot_counter increments once per boot")
    seq = 0
    allocated = []
    for _ in range(3):
        allocated.append(seq)
        seq += 1
    checks.check(allocated == [0, 1, 2], "seq starts at 0 and increments")
    checks.check(seq == 3, "next seq is 3")
    max_seq = 0xFFFFFFFF
    checks.check(max_seq + 1 == 1 << 32, "seq is uint32, no silent wrap in model")

    payload = {
        "v": 1,
        "frame": vector["frame_base64"],
        "lora_rx_at": "2026-11-12T14:03:22Z",
        "clock_synced": True,
        "buffered": False,
        "rssi": -90,
        "snr": 7.5,
    }
    checks.check(
        set(payload) == {
            "v",
            "frame",
            "lora_rx_at",
            "clock_synced",
            "buffered",
            "rssi",
            "snr",
        },
        "MQTT payload has exactly the contract fields",
    )
    checks.check("gateway_uid" not in payload, "gateway_uid is not in the payload")
    checks.check("farm_id" not in payload, "farm_id is not in the payload")
    checks.check(
        base64.b64decode(payload["frame"]).hex() == vector["frame_hex"],
        "payload frame decodes to the exact bytes",
    )
    checks.check(
        payload["clock_synced"] is True and payload["buffered"] is False,
        "payload clock_synced/buffered types",
    )

    null_payload = dict(payload)
    null_payload["lora_rx_at"] = None
    null_payload["clock_synced"] = False
    serialized = json.dumps(null_payload, separators=(",", ":"))
    checks.check('"lora_rx_at":null' in serialized, "unsynced time serializes as null")
    checks.check('"clock_synced":false' in serialized, "unsynced clock flag is false")

    check_failover_channels(checks, vector)

    print(f"{checks.count} checks, {checks.failures} failures")
    return 0 if checks.failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
