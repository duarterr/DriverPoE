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
# Must match ADMIN_PROTO_VERSION in admin_protocol.h. A response with any
# other version is rejected (ProtocolVersionMismatchError).
PROTO_VERSION = 5
SERIAL_LEN = 24
NONCE_LEN = 16
HMAC_LEN = 32
GCM_TAG_LEN = 16

_HEADER_FMT = ">IBB24s16sH"  # magic, version, type, serial, nonce, payload_len
HEADER_SIZE = struct.calcsize(_HEADER_FMT)
assert HEADER_SIZE == 48, HEADER_SIZE

DEFAULT_PORT = 5001
DEFAULT_TIMEOUT = 3.0
DEFAULT_RAMP_MS = 250  # filler for the ON/OFF/DIM ramp_ms wire field -- the firmware
#                        applies every level at once and ignores it (fades belong to
#                        the DMX layer / console).

# OTA -- must match admin_protocol.h's ADMIN_OTA_* constants exactly.
OTA_CHUNK_MAX_DATA = 1024  # bytes of image data per OTA_CHUNK packet
OTA_SHA256_LEN = 32

ZERO_NONCE = b"\x00" * NONCE_LEN

INFO_RESP_PAYLOAD_SIZE = 72       # 48 core + 16 DMX status + 8 dimming-mode
INFO_RESP_MIN_PAYLOAD_SIZE = 64   # firmware without the dimming block (pre-1.3.0); still discoverable so it can be OTA'd

# DMX layer config wire format -- must match DMX_CFG_WIRE_SIZE /
# dmx_config_pack() in components/dmx_input/include/dmx_input.h. 18 bytes:
# 1 layout-version byte + 17 payload bytes, multi-byte fields big-endian.
DMX_CFG_WIRE_SIZE = 18
DMX_CFG_LAYOUT_VERSION = 1

DMX_PROTO_ARTNET = 0x01
DMX_PROTO_SACN = 0x02

DMX_PERSONALITY_NAMES = {0: "1ch-8bit", 1: "2ch-16bit"}
DMX_MERGE_NAMES = {0: "HTP", 1: "LTP"}
DMX_LOSS_NAMES = {0: "hold", 1: "to-black", 2: "to-level"}
DMX_SOURCE_NAMES = {0: "none", 1: "artnet", 2: "sacn", 3: "both"}

# HV9910 dimming config wire format -- must match DRV_CFG_WIRE_SIZE /
# driver_config_pack() in components/driver_config/include/driver_config.h.
# 11 bytes: [0]=layout [1]=mode [2:4]=pwm_freq_hz(u16) [4:8]=analog_freq_hz(u32)
# [8:10]=min_on_time_us(u16) [10]=crossover_pct, multi-byte fields big-endian.
DRV_CFG_WIRE_SIZE = 11
DRV_CFG_LAYOUT_VERSION = 1

DRIVER_MODE_NAMES = {0: "pwm", 1: "analog", 2: "hybrid"}


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
    DMX_GET_CONFIG = 0x0E
    DMX_GET_CONFIG_RESP = 0x8E
    DMX_SET_CONFIG = 0x0F
    DMX_SET_CONFIG_RESP = 0x8F
    DRIVER_GET_CONFIG = 0x10
    DRIVER_GET_CONFIG_RESP = 0x90
    DRIVER_SET_CONFIG = 0x11
    DRIVER_SET_CONFIG_RESP = 0x91
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
    DeviceInfo adds the serial (from the packet header) and source_ip
    (from the socket) that aren't part of this payload itself."""
    if len(payload) not in (INFO_RESP_MIN_PAYLOAD_SIZE, INFO_RESP_PAYLOAD_SIZE):
        raise ProtocolError(
            f"unexpected INFO_RESP payload size: {len(payload)} "
            f"(expected {INFO_RESP_MIN_PAYLOAD_SIZE} or {INFO_RESP_PAYLOAD_SIZE})")
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
    out = {
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

    # DMX layer status block (payload[48:64]).
    b = payload[48:64]
    out.update({
        "dmx_layer_enabled": bool(b[0]),
        "dmx_active_source": DMX_SOURCE_NAMES.get(b[1], str(b[1])),
        "dmx_level": b[2],
        "dmx_fps": b[3],
        "dmx_artnet_port_address": struct.unpack(">H", b[4:6])[0],
        "dmx_sacn_universe": struct.unpack(">H", b[6:8])[0],
        "dmx_last_src_ip": ".".join(str(x) for x in b[8:12]),
        "dmx_address": struct.unpack(">H", b[12:14])[0],
        "dmx_personality": b[14],
        "dmx_proto_mask": b[15],
    })

    # Dimming-mode block (payload[64:72]); analog_freq_hz is in units of 10 Hz.
    # Absent on pre-1.3.0 firmware -- report zeros so DeviceInfo still builds
    # (the unit is still discoverable and can be OTA'd to a build that has it).
    if len(payload) >= INFO_RESP_PAYLOAD_SIZE:
        d = payload[64:72]
        out.update({
            "dimming_mode": d[0],
            "dimming_pwm_freq_hz": struct.unpack(">H", d[1:3])[0],
            "dimming_analog_freq_hz": struct.unpack(">H", d[3:5])[0] * 10,
            "dimming_min_on_time_us": struct.unpack(">H", d[5:7])[0],
            "dimming_crossover_pct": d[7],
        })
    else:
        out.update({
            "dimming_mode": 0, "dimming_pwm_freq_hz": 0, "dimming_analog_freq_hz": 0,
            "dimming_min_on_time_us": 0, "dimming_crossover_pct": 0,
        })
    return out


# ======================================================================= #
# DMX layer config -- must match dmx_config_pack()/dmx_config_unpack() in
# components/dmx_input/dmx_input.c exactly (DMX_CFG_WIRE_SIZE bytes,
# big-endian, leading layout-version byte).
# ======================================================================= #
@dataclass
class DmxConfig:
    layer_enabled: bool = False
    proto_mask: int = DMX_PROTO_ARTNET | DMX_PROTO_SACN
    artnet_port_address: int = 0        # 15-bit (net<<8)|(subnet<<4)|universe
    sacn_universe: int = 1             # 1..63999
    dmx_address: int = 1              # 1..512
    personality: int = 0             # 0 = 1ch 8-bit, 1 = 2ch 16-bit
    merge_mode: int = 0             # 0 = HTP, 1 = LTP
    loss_behavior: int = 0         # 0 = hold, 1 = to-black, 2 = to-level
    loss_level: int = 0           # 0..100, used by to-level
    loss_timeout_ms: int = 3000
    smoothing_ms: int = 25
    allow_artaddress: bool = True

    # Convenience views on the packed Art-Net port address.
    @property
    def artnet_net(self) -> int:
        return (self.artnet_port_address >> 8) & 0x7F

    @property
    def artnet_subnet(self) -> int:
        return (self.artnet_port_address >> 4) & 0x0F

    @property
    def artnet_universe(self) -> int:
        return self.artnet_port_address & 0x0F

    @staticmethod
    def from_artnet_parts(net: int, subnet: int, universe: int) -> int:
        """Packs net/subnet/universe into a 15-bit port address."""
        return ((net & 0x7F) << 8) | ((subnet & 0x0F) << 4) | (universe & 0x0F)


def pack_dmx_config(cfg: DmxConfig) -> bytes:
    return struct.pack(
        ">BBBHHHBBBBHHB",
        DMX_CFG_LAYOUT_VERSION,
        1 if cfg.layer_enabled else 0,
        cfg.proto_mask & 0xFF,
        cfg.artnet_port_address & 0x7FFF,
        cfg.sacn_universe & 0xFFFF,
        cfg.dmx_address & 0xFFFF,
        cfg.personality & 0xFF,
        cfg.merge_mode & 0xFF,
        cfg.loss_behavior & 0xFF,
        cfg.loss_level & 0xFF,
        cfg.loss_timeout_ms & 0xFFFF,
        cfg.smoothing_ms & 0xFFFF,
        1 if cfg.allow_artaddress else 0,
    )


def parse_dmx_config(payload: bytes) -> DmxConfig:
    if len(payload) != DMX_CFG_WIRE_SIZE:
        raise ProtocolError(
            f"unexpected DMX config size: {len(payload)} (expected {DMX_CFG_WIRE_SIZE})")
    (version, enabled, proto_mask, artnet_pa, sacn_u, addr, personality,
     merge, loss_beh, loss_lvl, loss_to, smooth, allow_aa) = struct.unpack(">BBBHHHBBBBHHB", payload)
    if version != DMX_CFG_LAYOUT_VERSION:
        raise ProtocolError(f"unsupported DMX config layout version {version}")
    return DmxConfig(
        layer_enabled=bool(enabled),
        proto_mask=proto_mask,
        artnet_port_address=artnet_pa,
        sacn_universe=sacn_u,
        dmx_address=addr,
        personality=personality,
        merge_mode=merge,
        loss_behavior=loss_beh,
        loss_level=loss_lvl,
        loss_timeout_ms=loss_to,
        smoothing_ms=smooth,
        allow_artaddress=bool(allow_aa),
    )


# ======================================================================= #
# HV9910 dimming config -- must match driver_config_pack()/_unpack() in
# components/driver_config/driver_config.c exactly (DRV_CFG_WIRE_SIZE
# bytes, big-endian, leading layout-version byte).
# ======================================================================= #
@dataclass
class DriverConfig:
    mode: int = 2                # 0 = pwm, 1 = analog, 2 = hybrid
    pwm_freq_hz: int = 2000      # PWMD switching frequency, 1000..5000
    analog_freq_hz: int = 60000  # LD (RC-fed) PWM frequency, 40000..80000
    min_on_time_us: int = 20     # PWMD minimum conduction burst, 2..200
    crossover_pct: int = 20      # hybrid knee, 10..60


def pack_driver_config(cfg: DriverConfig) -> bytes:
    return struct.pack(
        ">BBHIHB",
        DRV_CFG_LAYOUT_VERSION,
        cfg.mode & 0xFF,
        cfg.pwm_freq_hz & 0xFFFF,
        cfg.analog_freq_hz & 0xFFFFFFFF,
        cfg.min_on_time_us & 0xFFFF,
        cfg.crossover_pct & 0xFF,
    )


def parse_driver_config(payload: bytes) -> DriverConfig:
    if len(payload) != DRV_CFG_WIRE_SIZE:
        raise ProtocolError(
            f"unexpected driver config size: {len(payload)} (expected {DRV_CFG_WIRE_SIZE})")
    version, mode, pwm_hz, analog_hz, min_on_us, xover = struct.unpack(">BBHIHB", payload)
    if version != DRV_CFG_LAYOUT_VERSION:
        raise ProtocolError(f"unsupported driver config layout version {version}")
    return DriverConfig(
        mode=mode,
        pwm_freq_hz=pwm_hz,
        analog_freq_hz=analog_hz,
        min_on_time_us=min_on_us,
        crossover_pct=xover,
    )
