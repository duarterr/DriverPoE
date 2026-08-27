"""Wire protocol: constants, packet (de)serialization, device identity
helpers. Must match components/admin_channel/admin_protocol.h and
components/devid/devid.c exactly -- see the comments below for which C
side each piece mirrors.

Pure and I/O-free on purpose (no sockets, no files, no print()/input()) --
this module only knows how to turn Python values into bytes and back. All
network I/O lives in client.py/discovery.py.
"""
from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass, field
from enum import IntEnum

# --------------------------------------------------------------------- #
# Identity constants -- must match components/devid/devid.h exactly.
# --------------------------------------------------------------------- #
MODEL_PREFIX = "DriverPoE"
SECRET_LEN = 32
MAC_LEN = 6

# --------------------------------------------------------------------- #
# Admin channel wire format -- must match
# components/admin_channel/admin_protocol.h exactly.
# --------------------------------------------------------------------- #
MAGIC = 0x44504F45  # "DPOE"
# 4: adds OTA (PacketType.OTA_*) over this same channel -- see
# admin_protocol.h's own version comment. Nothing about the pre-existing
# commands changed; the version bump is because ADMIN_MAX_PAYLOAD grew
# (to fit one OTA chunk) and a v3 client wouldn't understand the new
# packet types anyway.
PROTO_VERSION = 4
SERIAL_LEN = 24
NONCE_LEN = 16
HMAC_LEN = 32
GCM_TAG_LEN = 16

_HEADER_FMT = ">IBB24s16sH"  # magic, version, type, serial, nonce, payload_len
HEADER_SIZE = struct.calcsize(_HEADER_FMT)
assert HEADER_SIZE == 48, HEADER_SIZE

DEFAULT_PORT = 5001
DEFAULT_TIMEOUT = 3.0
DEFAULT_RAMP_MS = 250  # must match HV9910_DEFAULT_RAMP_MS in main/poe_luminaire_main.h

# OTA -- must match admin_protocol.h's ADMIN_OTA_* constants exactly.
OTA_CHUNK_MAX_DATA = 1024  # bytes of image data per OTA_CHUNK packet
OTA_SHA256_LEN = 32

ZERO_NONCE = b"\x00" * NONCE_LEN

INFO_RESP_PAYLOAD_SIZE = 48  # see parse_info_payload() below


class PacketType(IntEnum):
    INFO = 0x01
    INFO_RESP = 0x81
    CHALLENGE = 0x02
    CHALLENGE_RESP = 0x82
    ON = 0x03
    ON_RESP = 0x83
    OFF = 0x04
    OFF_RESP = 0x84
    DIM = 0x05
    DIM_RESP = 0x85
    IDENTIFY = 0x06
    IDENTIFY_RESP = 0x86
    REBOOT = 0x07
    REBOOT_RESP = 0x87
    FACTORY_RESET = 0x08
    FACTORY_RESET_RESP = 0x88
    CHANGE_SECRET = 0x09
    CHANGE_SECRET_RESP = 0x89
    OTA_BEGIN = 0x0A
    OTA_BEGIN_RESP = 0x8A
    OTA_CHUNK = 0x0B
    OTA_CHUNK_RESP = 0x8B
    OTA_END = 0x0C
    OTA_END_RESP = 0x8C
    OTA_ABORT = 0x0D
    OTA_ABORT_RESP = 0x8D
    ERR_RESP = 0xFF


class AdminStatus(IntEnum):
    OK = 0                    # accepted AND applied immediately
    ERR_BAD_ARG = 1
    ERR_NOT_READY = 2
    ERR_INTERNAL = 3
    ACCEPTED_PENDING = 4       # accepted, intent persisted, applied later (see ON/DIM)
    ERR_BUSY = 5               # another OTA session is already in progress (see OTA_BEGIN)


def status_name(code: int) -> str:
    try:
        return AdminStatus(code).name
    except ValueError:
        return f"UNKNOWN({code})"


# ======================================================================= #
# Errors
# ======================================================================= #
class ProtocolError(Exception):
    """A response was malformed, truncated, or otherwise didn't parse --
    never raised for network-level failures (see client.py's
    DeviceTimeoutError) or authentication failures (see client.py's
    AuthError)."""


class ProtocolVersionMismatchError(ProtocolError):
    """The device speaks a different ADMIN_PROTO_VERSION than this package
    does. Deliberately a distinct type from ProtocolError so callers (the
    CLI, the web UI) can show "firmware speaks vX, this tool speaks vY --
    update one of them" instead of a generic parse error."""

    def __init__(self, got: int, expected: int = PROTO_VERSION):
        self.got = got
        self.expected = expected
        super().__init__(
            f"protocol version mismatch: device speaks v{got}, this package speaks v{expected}"
        )


# ======================================================================= #
# Device identity
# ======================================================================= #
def mac_from_str(s: str) -> bytes:
    """Accepts 'AA:BB:CC:DD:EE:FF', 'AA-BB-CC-DD-EE-FF', or 'AABBCCDDEEFF'."""
    cleaned = s.replace(":", "").replace("-", "").strip()
    if len(cleaned) != MAC_LEN * 2:
        raise ValueError(f"Invalid MAC: {s!r}")
    return bytes.fromhex(cleaned)


def mac_to_str(mac: bytes) -> str:
    return ":".join(f"{b:02X}" for b in mac)


def serial_from_mac(mac: bytes, model: str = MODEL_PREFIX) -> str:
    if len(mac) != MAC_LEN:
        raise ValueError("MAC must be 6 bytes")
    return f"{model}-{mac.hex().upper()}"


def mac_from_serial(serial: str) -> bytes:
    """Extracts the MAC back out of a "MODEL-XXXXXXXXXXXX" serial."""
    _, _, hexpart = serial.rpartition("-")
    if len(hexpart) != MAC_LEN * 2:
        raise ValueError(f"Serial doesn't have the expected format (MODEL-<12 hex>): {serial!r}")
    return bytes.fromhex(hexpart)


# ======================================================================= #
# Binary packet
# ======================================================================= #
@dataclass
class Packet:
    type: int
    serial: str
    nonce: bytes
    payload: bytes
    # Filled in by unpack(), used by verify_hmac() -- not part of the
    # packet's logical content.
    _covered: bytes = field(default=b"", repr=False, compare=False)
    _mac: bytes = field(default=b"", repr=False, compare=False)

    def header_bytes(self, payload_len: int | None = None) -> bytes:
        """Builds just the 48-byte header (no payload, no HMAC). Used both
        by pack() and as the AES-GCM AAD in change-secret (in that case
        the payload doesn't exist yet -- the final ciphertext length is
        already known, so the header can be built before encrypting)."""
        serial_b = self.serial.encode("ascii")[:SERIAL_LEN].ljust(SERIAL_LEN, b"\x00")
        nonce_b = (self.nonce or ZERO_NONCE).ljust(NONCE_LEN, b"\x00")[:NONCE_LEN]
        plen = len(self.payload) if payload_len is None else payload_len
        return struct.pack(_HEADER_FMT, MAGIC, PROTO_VERSION, int(self.type),
                            serial_b, nonce_b, plen)

    def pack(self, hmac_key: bytes | None) -> bytes:
        body = self.header_bytes() + self.payload
        mac = hmac.new(hmac_key, body, hashlib.sha256).digest() if hmac_key is not None else b"\x00" * HMAC_LEN
        return body + mac

    @staticmethod
    def unpack(data: bytes) -> "Packet":
        if len(data) < HEADER_SIZE + HMAC_LEN:
            raise ProtocolError(f"packet too short ({len(data)} bytes)")
        magic, version, ptype, serial_b, nonce_b, payload_len = struct.unpack_from(_HEADER_FMT, data, 0)
        if magic != MAGIC:
            raise ProtocolError(f"invalid magic: {magic:#x}")
        if version != PROTO_VERSION:
            raise ProtocolVersionMismatchError(got=version)
        expected_len = HEADER_SIZE + payload_len + HMAC_LEN
        if len(data) != expected_len:
            raise ProtocolError(f"inconsistent size: expected {expected_len}, got {len(data)}")

        payload = data[HEADER_SIZE:HEADER_SIZE + payload_len]
        covered = data[:HEADER_SIZE + payload_len]
        mac = data[HEADER_SIZE + payload_len:]
        serial = serial_b.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        return Packet(type=ptype, serial=serial, nonce=nonce_b, payload=payload,
                      _covered=covered, _mac=mac)

    def verify_hmac(self, hmac_key: bytes) -> bool:
        expected = hmac.new(hmac_key, self._covered, hashlib.sha256).digest()
        return hmac.compare_digest(expected, self._mac)


def status_byte(payload: bytes) -> int:
    if len(payload) < 1:
        raise ProtocolError("empty response payload -- expected 1 status byte")
    return payload[0]


# ======================================================================= #
# INFO_RESP payload -- must match handle_info() in
# components/admin_channel/admin_channel.c exactly (48 bytes, v3, no
# Matter fields).
# ======================================================================= #
_POE_SOURCE_NAMES = {0: "none", 1: "type1", 2: "type2", 3: "aux"}
_RESET_REASON_NAMES = {
    0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC", 5: "INT_WDT",
    6: "TASK_WDT", 7: "WDT", 8: "DEEPSLEEP", 9: "BROWNOUT", 10: "SDIO",
    11: "USB", 12: "JTAG", 13: "EFUSE", 14: "PWR_GLITCH", 15: "CPU_LOCKUP",
}


def parse_info_payload(payload: bytes) -> dict:
    """Returns a plain dict of the fields, in wire order -- models.py's
    DeviceInfo.from_wire() adds the serial (from the packet header) and
    source_ip (from the socket) that aren't part of this payload itself."""
    if len(payload) != INFO_RESP_PAYLOAD_SIZE:
        raise ProtocolError(f"unexpected INFO_RESP payload size: {len(payload)} (expected {INFO_RESP_PAYLOAD_SIZE})")
    mac = payload[0:6]
    fw_version = payload[6:22].split(b"\x00", 1)[0].decode("ascii", errors="replace")
    ip = ".".join(str(b) for b in payload[22:26])
    uptime_s = struct.unpack(">I", payload[26:30])[0]
    reset_reason = payload[30]
    poe_ready = bool(payload[31])
    poe_source = payload[32]
    poe_cdb_confirmed = bool(payload[33])
    poe_t2p_confirmed = bool(payload[34])
    poe_vbus_confirmed = bool(payload[35])
    driver_on = bool(payload[36])
    desired_on = bool(payload[37])
    dim_percent = payload[38]
    ramp_pending = bool(payload[39])
    vbus_mv = struct.unpack(">I", payload[40:44])[0]
    led_voltage_mv = struct.unpack(">i", payload[44:48])[0]
    return {
        "mac": mac,
        "fw_version": fw_version,
        "ip": ip,
        "uptime_s": uptime_s,
        "reset_reason": _RESET_REASON_NAMES.get(reset_reason, str(reset_reason)),
        "poe_ready": poe_ready,
        "poe_source": _POE_SOURCE_NAMES.get(poe_source, str(poe_source)),
        "poe_cdb_confirmed": poe_cdb_confirmed,
        "poe_t2p_confirmed": poe_t2p_confirmed,
        "poe_vbus_confirmed": poe_vbus_confirmed,
        "driver_on": driver_on,
        "desired_on": desired_on,
        "dim_percent": dim_percent,
        "ramp_pending": ramp_pending,
        "vbus_mv": vbus_mv,
        "led_voltage_mv": led_voltage_mv,
    }
