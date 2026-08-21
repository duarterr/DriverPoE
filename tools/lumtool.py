#!/usr/bin/env python3
"""
DriverPoE admin tool -- single self-contained script (this is the
ONLY .py file in this directory, tests included -- see the bottom of the
file). No imports from sibling files. Interactive menu only (no
argparse/CLI flags): run it with no arguments and pick an action.
Run `python -m unittest lumtool -v` from inside tools/ for the test
suite instead -- see the comment above the tests for why the two never
collide.

Covers device identity, provisioning, and remote administration end to
end:
  - HKDF-SHA256 (RFC 5869) key derivation, matching components/devid/devid.h exactly.
  - The admin channel's binary packet format (components/admin_channel/admin_protocol.h).
  - Provisioning a fresh unit entirely over the network (CLAIM, see
    README.md "Provisionar sem debugger") -- no esptool/serial connection
    involved at all, on purpose: this tool never touches flash directly.
    CLAIM is unauthenticated, the same as DISCOVER -- see do_claim().
  - The authenticated UDP admin commands: discover, status, on/off/dim,
    identify, reboot, factory reset, key rotation. This channel is the
    ONLY network control surface the firmware exposes -- there's no
    separate unauthenticated text/TCP command server anymore.

Master secret: read from DEFAULT_SECRET_FILE ("secret.txt") by default,
or typed in directly instead (input hidden via getpass). Never accepted
as a command-line argument (would end up in shell history / `ps`).

Standard library only, except `cryptography` (needed for "rotate admin
key" -- there's no responsible way to do AES-GCM in pure Python). Run
`pip install cryptography` if you need that one.
"""
from __future__ import annotations

import csv
import datetime
import getpass
import hashlib
import hmac
import os
import socket
import stat
import struct
import time
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:  # only needed for "rotate admin key"
    AESGCM = None

# --------------------------------------------------------------------- #
# Identity / key derivation constants -- must match components/devid/devid.h exactly.
# --------------------------------------------------------------------- #
MODEL_PREFIX = "DriverPoE"
KEY_LEN = 32
MAC_LEN = 6

DEFAULT_SECRET_FILE = Path(__file__).resolve().parent / "secret.txt"

# --------------------------------------------------------------------- #
# Admin channel wire format -- must match components/admin_channel/admin_protocol.h exactly.
# --------------------------------------------------------------------- #
MAGIC = 0x44504F45  # "DPOE"
PROTO_VERSION = 1
SERIAL_LEN = 24
NONCE_LEN = 16
HMAC_LEN = 32
GCM_TAG_LEN = 16

_HEADER_FMT = ">IBB24sI16sH"
HEADER_SIZE = struct.calcsize(_HEADER_FMT)
assert HEADER_SIZE == 52, HEADER_SIZE

DEFAULT_PORT = 5001
DEFAULT_TIMEOUT = 3.0
DEFAULT_RAMP_MS = 250  # must match HV9910_DEFAULT_RAMP_MS in main/poe_luminaire_main.h
FALLBACK_BROADCAST = "255.255.255.255"  # used only if the guess below fails


def guess_broadcast_address() -> str:
    """Best-effort guess of this machine's local broadcast address, from
    whatever interface the OS would pick to reach the internet -- assumes
    a /24 subnet (true for the overwhelming majority of the small LANs
    this tool runs on). Shown as the editable default in the prompt, not
    used blindly -- the operator can always type a different one."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(0.5)
            s.connect(("8.8.8.8", 80))  # UDP "connect" -- no packet actually sent
            local_ip = s.getsockname()[0]
        return ".".join(local_ip.split(".")[:3]) + ".255"
    except OSError:
        return FALLBACK_BROADCAST

# --------------------------------------------------------------------- #
# Production log -- written by CLAIM (the only provisioning path left,
# see README.md "Provisionar sem debugger"). Never includes the key.
# --------------------------------------------------------------------- #
CSV_LOG_HEADER = ["serial", "mac", "model", "epoch", "fw_version", "timestamp", "operator"]
DEFAULT_PRODUCTION_LOG = Path(__file__).resolve().parent / "production_log.csv"


class PacketType(IntEnum):
    DISCOVER = 0x01
    DISCOVER_RESP = 0x81
    CHALLENGE = 0x02
    CHALLENGE_RESP = 0x82
    STATUS = 0x03
    STATUS_RESP = 0x83
    IDENTIFY = 0x04
    IDENTIFY_RESP = 0x84
    REBOOT = 0x05
    REBOOT_RESP = 0x85
    FACTORY_RESET = 0x06
    FACTORY_RESET_RESP = 0x86
    ROTATE_KEY = 0x07
    ROTATE_KEY_RESP = 0x87
    ROTATE_CONFIRM = 0x08
    ROTATE_CONFIRM_RESP = 0x88
    CLAIM = 0x09
    CLAIM_RESP = 0x89
    ON = 0x0A
    ON_RESP = 0x8A
    OFF = 0x0B
    OFF_RESP = 0x8B
    DIM = 0x0C
    DIM_RESP = 0x8C
    ERR_RESP = 0xFF


class AdminStatus(IntEnum):
    OK = 0
    ERR_BAD_ARG = 1
    ERR_NOT_READY = 2
    ERR_NOT_PROVISIONED = 3
    ERR_NO_STAGED_KEY = 4
    ERR_INTERNAL = 5


ZERO_NONCE = b"\x00" * NONCE_LEN


# ======================================================================= #
# Master secret
# ======================================================================= #
def _check_file_permissions(path: str) -> None:
    """Aborts if the secret file is readable/writable by group or others.
    Only meaningful on POSIX -- Windows has no such permission model, so
    the check is skipped there (protect the file some other way, e.g. an
    NTFS ACL)."""
    if os.name != "posix":
        return
    mode = stat.S_IMODE(os.stat(path).st_mode)
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise SystemExit(f"{path} is readable/writable by group or others ({oct(mode)}). Run: chmod 600 {path}")


def _secret_from_file(path: Path, label: str) -> bytes:
    _check_file_permissions(str(path))
    raw = path.read_bytes().strip()
    try:
        secret = bytes.fromhex(raw.decode("ascii"))
    except (ValueError, UnicodeDecodeError):
        secret = raw  # raw bytes, not hex
    if len(secret) != KEY_LEN:
        raise SystemExit(f"{label} in {path} must be {KEY_LEN} bytes; got {len(secret)}.")
    return secret


def _secret_from_prompt(label: str) -> bytes:
    raw = getpass.getpass(f"{label} (64 hex chars, input hidden): ").strip()
    try:
        secret = bytes.fromhex(raw)
    except ValueError:
        raise SystemExit(f"{label} must be a 64-character hex string.")
    if len(secret) != KEY_LEN:
        raise SystemExit(f"{label} must be {KEY_LEN} bytes; got {len(secret)}.")
    return secret


def _load_secret(default_file: Path, label: str, offer_choice: bool) -> bytes:
    """Loads a secret from a file, or lets the operator type it in
    instead. Used for the master secret (see get_master_secret()). With
    offer_choice=True, asks the operator up front whether to read
    default_file or type the value in right now. Otherwise silently
    prefers default_file and falls back to prompting only if it's
    missing. Never accepted as a command-line argument."""
    if offer_choice:
        print(f"{label} source:")
        print(f"  [1] Read from {default_file} (default)")
        print("  [2] Type it now (input hidden, not saved anywhere)")
        choice = input("Choice [1]: ").strip() or "1"
        if choice == "2":
            return _secret_from_prompt(label)
        if not default_file.exists():
            raise SystemExit(f"{default_file} doesn't exist.")
        return _secret_from_file(default_file, label)

    if default_file.exists():
        return _secret_from_file(default_file, label)
    print(f"{default_file} not found.")
    return _secret_from_prompt(label)


def get_master_secret(offer_choice: bool = False) -> bytes:
    """Loads the 32-byte master secret used to derive real per-unit admin
    keys (see derive_admin_key()). See _load_secret() for the source
    rules."""
    return _load_secret(DEFAULT_SECRET_FILE, "Master secret", offer_choice)


# ======================================================================= #
# HKDF-SHA256 (RFC 5869)
# ======================================================================= #
def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    okm = b""
    t = b""
    counter = 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        okm += t
        counter += 1
    return okm[:length]


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int = 32) -> bytes:
    return hkdf_expand(hkdf_extract(salt, ikm), info, length)


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
    """Extracts the MAC back out of a "MODEL-XXXXXXXXXXXX" serial -- lets
    us recover the MAC (to derive the key) from what DISCOVER_RESP
    already returns, with no separate payload field needed."""
    _, _, hexpart = serial.rpartition("-")
    if len(hexpart) != MAC_LEN * 2:
        raise ValueError(f"Serial doesn't have the expected format (MODEL-<12 hex>): {serial!r}")
    return bytes.fromhex(hexpart)


def build_hkdf_info(model: str, epoch: int) -> bytes:
    """"lum-admin-v1" || model || epoch (uint32 big-endian) -- must match
    what README.md documents exactly. The "lum-admin-v1" label is a frozen
    HKDF domain separator, independent of MODEL_PREFIX/DEVID_MODEL_PREFIX
    -- see TestDeriveAdminKey.test_golden_vector below for why it must
    never change casually."""
    return b"lum-admin-v1" + model.encode("ascii") + struct.pack(">I", epoch)


def derive_admin_key(master_secret: bytes, mac: bytes, epoch: int, model: str = MODEL_PREFIX) -> bytes:
    info = build_hkdf_info(model, epoch)
    return hkdf_sha256(master_secret, mac, info, KEY_LEN)


# ======================================================================= #
# Binary packet (components/admin_channel/admin_protocol.h)
# ======================================================================= #
@dataclass
class Packet:
    type: int
    serial: str
    epoch: int
    nonce: bytes
    payload: bytes
    # Filled in by unpack(), used by verify_hmac() -- not part of the
    # packet's logical content.
    _covered: bytes = field(default=b"", repr=False, compare=False)
    _mac: bytes = field(default=b"", repr=False, compare=False)

    def header_bytes(self, payload_len: int | None = None) -> bytes:
        """Builds just the 52-byte header (no payload, no HMAC). Used both
        by pack() and as the AES-GCM AAD in rotate-key (in that case the
        payload doesn't exist yet -- the final ciphertext length is
        already known, so the header can be built before encrypting)."""
        serial_b = self.serial.encode("ascii")[:SERIAL_LEN].ljust(SERIAL_LEN, b"\x00")
        nonce_b = (self.nonce or ZERO_NONCE).ljust(NONCE_LEN, b"\x00")[:NONCE_LEN]
        plen = len(self.payload) if payload_len is None else payload_len
        return struct.pack(_HEADER_FMT, MAGIC, PROTO_VERSION, int(self.type),
                            serial_b, self.epoch, nonce_b, plen)

    def pack(self, hmac_key: bytes | None) -> bytes:
        body = self.header_bytes() + self.payload
        mac = hmac.new(hmac_key, body, hashlib.sha256).digest() if hmac_key is not None else b"\x00" * HMAC_LEN
        return body + mac

    @staticmethod
    def unpack(data: bytes) -> "Packet":
        if len(data) < HEADER_SIZE + HMAC_LEN:
            raise ValueError(f"packet too short ({len(data)} bytes)")
        magic, version, ptype, serial_b, epoch, nonce_b, payload_len = struct.unpack_from(_HEADER_FMT, data, 0)
        if magic != MAGIC:
            raise ValueError(f"invalid magic: {magic:#x}")
        if version != PROTO_VERSION:
            raise ValueError(f"invalid protocol version: {version}")
        expected_len = HEADER_SIZE + payload_len + HMAC_LEN
        if len(data) != expected_len:
            raise ValueError(f"inconsistent size: expected {expected_len}, got {len(data)}")

        payload = data[HEADER_SIZE:HEADER_SIZE + payload_len]
        covered = data[:HEADER_SIZE + payload_len]
        mac = data[HEADER_SIZE + payload_len:]
        serial = serial_b.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        return Packet(type=ptype, serial=serial, epoch=epoch, nonce=nonce_b, payload=payload,
                      _covered=covered, _mac=mac)

    def verify_hmac(self, hmac_key: bytes) -> bool:
        expected = hmac.new(hmac_key, self._covered, hashlib.sha256).digest()
        return hmac.compare_digest(expected, self._mac)


# ======================================================================= #
# Provisioning: production log only -- the actual identity write happens
# entirely over the network now, see do_claim() below.
# ======================================================================= #
def append_production_log(csv_path: Path, serial: str, mac: bytes, model: str,
                           epoch: int, fw_version: str, operator: str) -> None:
    is_new = not csv_path.exists()
    with open(csv_path, "a", newline="") as f:
        w = csv.writer(f)
        if is_new:
            w.writerow(CSV_LOG_HEADER)
        w.writerow([
            serial, mac_to_str(mac), model, str(epoch), fw_version,
            datetime.datetime.now().isoformat(timespec="seconds"), operator,
        ])


def do_derive(mac_str: str, epoch: int, model: str, secret: bytes) -> None:
    mac = mac_from_str(mac_str)
    key = derive_admin_key(secret, mac, epoch, model)
    print(f"Serial: {serial_from_mac(mac, model)}")
    print(f"Key (epoch {epoch}): {key.hex()}")


# ======================================================================= #
# Admin channel client (UDP)
# ======================================================================= #
class AdminClient:
    def __init__(self, ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT):
        self.addr = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def send(self, pkt: Packet, key: bytes | None) -> None:
        self.sock.sendto(pkt.pack(key), self.addr)

    def recv(self) -> Packet:
        data, _ = self.sock.recvfrom(4096)
        return Packet.unpack(data)

    def discover_one(self) -> Packet:
        pkt = Packet(type=PacketType.DISCOVER, serial="", epoch=0, nonce=b"", payload=b"")
        self.send(pkt, None)
        return self.recv()

    def challenge(self, key: bytes, serial: str, epoch: int) -> bytes:
        pkt = Packet(type=PacketType.CHALLENGE, serial=serial, epoch=epoch, nonce=b"", payload=b"")
        self.send(pkt, key)
        resp = self.recv()
        if resp.type != PacketType.CHALLENGE_RESP or not resp.verify_hmac(key):
            raise SystemExit("CHALLENGE failed (invalid HMAC -- wrong key/epoch?).")
        return resp.nonce


def broadcast_discover(bcast_ip: str, port: int, timeout: float) -> list[tuple[Packet, str]]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    pkt = Packet(type=PacketType.DISCOVER, serial="", epoch=0, nonce=b"", payload=b"")
    sock.sendto(pkt.pack(None), (bcast_ip, port))

    results = []
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            data, src = sock.recvfrom(4096)
        except socket.timeout:
            break
        try:
            resp = Packet.unpack(data)
        except ValueError:
            continue
        if resp.type == PacketType.DISCOVER_RESP:
            results.append((resp, src[0]))
    return results


def parse_discover_payload(payload: bytes) -> dict:
    if len(payload) != 41:
        raise ValueError(f"unexpected DISCOVER_RESP payload size: {len(payload)}")
    model = payload[0:16].split(b"\x00", 1)[0].decode("ascii", errors="replace")
    epoch = struct.unpack(">I", payload[16:20])[0]
    fw_version = payload[20:36].split(b"\x00", 1)[0].decode("ascii", errors="replace")
    ip = ".".join(str(b) for b in payload[36:40])
    provisioned = bool(payload[40])
    return {"model": model, "epoch": epoch, "fw_version": fw_version, "ip": ip, "provisioned": provisioned}


_POE_SOURCE_NAMES = {0: "none", 1: "type1", 2: "type2", 3: "aux"}
_RESET_REASON_NAMES = {
    0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC", 5: "INT_WDT",
    6: "TASK_WDT", 7: "WDT", 8: "DEEPSLEEP", 9: "BROWNOUT", 10: "SDIO",
    11: "USB", 12: "JTAG", 13: "EFUSE", 14: "PWR_GLITCH", 15: "CPU_LOCKUP",
}


def parse_status_payload(payload: bytes) -> dict:
    if len(payload) != 21:
        raise ValueError(f"unexpected STATUS_RESP payload size: {len(payload)}")
    uptime_s = struct.unpack(">I", payload[0:4])[0]
    reset_reason = payload[4]
    poe_ready = bool(payload[5])
    poe_source = payload[6]
    driver_on = bool(payload[7])
    dim = payload[8]
    vbus_mv = struct.unpack(">I", payload[9:13])[0]
    led_voltage_mv = struct.unpack(">i", payload[13:17])[0]
    ip = ".".join(str(b) for b in payload[17:21])
    return {
        "uptime_s": uptime_s,
        "reset_reason": _RESET_REASON_NAMES.get(reset_reason, str(reset_reason)),
        "poe_ready": poe_ready,
        "poe_source": _POE_SOURCE_NAMES.get(poe_source, str(poe_source)),
        "driver_on": driver_on,
        "dim": dim,
        "vbus_mv": vbus_mv,
        "led_voltage_mv": led_voltage_mv,
        "eth_ip": ip,
    }


def status_byte(payload: bytes) -> int:
    if len(payload) < 1:
        raise ValueError("empty response payload -- expected 1 status byte")
    return payload[0]


def status_name(code: int) -> str:
    try:
        return AdminStatus(code).name
    except ValueError:
        return f"UNKNOWN({code})"


def discover_and_derive(client: AdminClient, secret: bytes, model: str) -> tuple[Packet, dict, bytes]:
    disc = client.discover_one()
    info = parse_discover_payload(disc.payload)
    if not info["provisioned"]:
        raise SystemExit(f"{disc.serial}: device not provisioned -- only discover works.")
    mac = mac_from_serial(disc.serial)
    key = derive_admin_key(secret, mac, info["epoch"], model)
    return disc, info, key


# ======================================================================= #
# Admin actions
# ======================================================================= #
def do_status(ip: str, secret: bytes, model: str = MODEL_PREFIX,
              port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    pkt = Packet(type=PacketType.STATUS, serial=disc.serial, epoch=info["epoch"], nonce=b"", payload=b"")
    client.send(pkt, key)
    resp = client.recv()
    if not resp.verify_hmac(key):
        raise SystemExit("STATUS_RESP has an invalid HMAC.")
    st = parse_status_payload(resp.payload)
    print(f"Serial:        {disc.serial}")
    print(f"Epoch:         {info['epoch']}")
    print(f"Uptime:        {st['uptime_s']}s")
    print(f"Reset reason:  {st['reset_reason']}")
    print(f"PoE ready:     {st['poe_ready']} (source: {st['poe_source']})")
    print(f"Driver on:     {st['driver_on']} (dim={st['dim']}%)")
    print(f"VBUS:          {st['vbus_mv']}mV")
    print(f"LED voltage:   {st['led_voltage_mv']}mV")
    print(f"IP:            {st['eth_ip']}")


def do_identify(ip: str, secret: bytes, model: str = MODEL_PREFIX,
                port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    pkt = Packet(type=PacketType.IDENTIFY, serial=disc.serial, epoch=info["epoch"], nonce=b"", payload=b"")
    client.send(pkt, key)
    resp = client.recv()
    if not resp.verify_hmac(key):
        raise SystemExit("IDENTIFY_RESP has an invalid HMAC.")
    code = status_byte(resp.payload)
    if code == AdminStatus.OK:
        print(f"{disc.serial}: blinking for a few seconds.")
    else:
        raise SystemExit(f"{disc.serial}: IDENTIFY refused ({status_name(code)}).")


def do_on(ip: str, secret: bytes, ramp_ms: int = DEFAULT_RAMP_MS, model: str = MODEL_PREFIX,
          port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    pkt = Packet(type=PacketType.ON, serial=disc.serial, epoch=info["epoch"], nonce=b"",
                 payload=struct.pack(">I", ramp_ms))
    client.send(pkt, key)
    resp = client.recv()
    if resp.type != PacketType.ON_RESP or not resp.verify_hmac(key):
        raise SystemExit("ON failed (invalid response or wrong HMAC).")
    code = status_byte(resp.payload)
    if code != AdminStatus.OK:
        raise SystemExit(f"ON refused: {status_name(code)}")
    print(f"{disc.serial}: turning on, ramp={ramp_ms}ms.")


def do_off(ip: str, secret: bytes, ramp_ms: int = DEFAULT_RAMP_MS, model: str = MODEL_PREFIX,
           port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    pkt = Packet(type=PacketType.OFF, serial=disc.serial, epoch=info["epoch"], nonce=b"",
                 payload=struct.pack(">I", ramp_ms))
    client.send(pkt, key)
    resp = client.recv()
    if resp.type != PacketType.OFF_RESP or not resp.verify_hmac(key):
        raise SystemExit("OFF failed (invalid response or wrong HMAC).")
    code = status_byte(resp.payload)
    if code != AdminStatus.OK:
        raise SystemExit(f"OFF refused: {status_name(code)}")
    print(f"{disc.serial}: turning off, ramp={ramp_ms}ms.")


def do_dim(ip: str, secret: bytes, percent: int, ramp_ms: int = DEFAULT_RAMP_MS, model: str = MODEL_PREFIX,
           port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    if not 0 <= percent <= 100:
        raise SystemExit(f"percent must be 0-100, got {percent}.")

    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    pkt = Packet(type=PacketType.DIM, serial=disc.serial, epoch=info["epoch"], nonce=b"",
                 payload=bytes([percent]) + struct.pack(">I", ramp_ms))
    client.send(pkt, key)
    resp = client.recv()
    if resp.type != PacketType.DIM_RESP or not resp.verify_hmac(key):
        raise SystemExit("DIM failed (invalid response or wrong HMAC).")
    code = status_byte(resp.payload)
    if code != AdminStatus.OK:
        raise SystemExit(f"DIM refused: {status_name(code)}")
    print(f"{disc.serial}: dimming to {percent}%, ramp={ramp_ms}ms.")


def _destructive_command(ip: str, secret: bytes, ptype: int, resp_type: int, label: str, model: str,
                          port: int, timeout: float) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    nonce = client.challenge(key, disc.serial, info["epoch"])
    pkt = Packet(type=ptype, serial=disc.serial, epoch=info["epoch"], nonce=nonce, payload=b"")
    client.send(pkt, key)
    resp = client.recv()
    if resp.type != resp_type or not resp.verify_hmac(key):
        raise SystemExit(f"{label} failed (invalid response or wrong HMAC).")
    code = status_byte(resp.payload)
    if code != AdminStatus.OK:
        raise SystemExit(f"{label} refused: {status_name(code)}")
    print(f"{disc.serial}: {label} confirmed by the device.")


def do_reboot(ip: str, secret: bytes, model: str = MODEL_PREFIX,
              port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    _destructive_command(ip, secret, PacketType.REBOOT, PacketType.REBOOT_RESP, "REBOOT", model, port, timeout)


def do_reset(ip: str, secret: bytes, model: str = MODEL_PREFIX,
             port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    client = AdminClient(ip, port, timeout)
    disc, info, key = discover_and_derive(client, secret, model)

    typed = input(f"Type the exact serial to confirm the FACTORY_RESET ({disc.serial}): ")
    if typed.strip() != disc.serial:
        raise SystemExit("Serial doesn't match -- aborted, nothing was sent.")

    nonce = client.challenge(key, disc.serial, info["epoch"])
    pkt = Packet(type=PacketType.FACTORY_RESET, serial=disc.serial, epoch=info["epoch"], nonce=nonce, payload=b"")
    client.send(pkt, key)
    resp = client.recv()
    if resp.type != PacketType.FACTORY_RESET_RESP or not resp.verify_hmac(key):
        raise SystemExit("FACTORY_RESET failed (invalid response or wrong HMAC).")
    code = status_byte(resp.payload)
    if code != AdminStatus.OK:
        raise SystemExit(f"FACTORY_RESET refused: {status_name(code)}")
    print(f"{disc.serial}: FACTORY_RESET confirmed -- serial/key/epoch preserved, everything else erased.")


def do_rotate_key(ip: str, secret: bytes, new_epoch: int | None, model: str = MODEL_PREFIX,
                   port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    if AESGCM is None:
        raise SystemExit("The 'cryptography' package isn't installed (pip install cryptography) -- required for this.")

    client = AdminClient(ip, port, timeout)
    disc, info, old_key = discover_and_derive(client, secret, model)

    old_epoch = info["epoch"]
    target_epoch = new_epoch if new_epoch is not None else old_epoch + 1
    if target_epoch <= old_epoch:
        raise SystemExit(f"The new epoch ({target_epoch}) must be greater than the current one ({old_epoch}).")

    mac = mac_from_serial(disc.serial)
    new_key = derive_admin_key(secret, mac, target_epoch, model)

    nonce = client.challenge(old_key, disc.serial, old_epoch)

    # Build the header BEFORE encrypting -- the payload size (52 bytes:
    # 36 bytes of plaintext + 16 of GCM tag) is already known, so the
    # full header can be used as the AAD (binds the new key to this
    # specific serial/epoch/nonce).
    plaintext = new_key + struct.pack(">I", target_epoch)
    header_stub = Packet(type=PacketType.ROTATE_KEY, serial=disc.serial, epoch=old_epoch, nonce=nonce, payload=b"")
    aad = header_stub.header_bytes(payload_len=len(plaintext) + 16)

    ciphertext = AESGCM(old_key).encrypt(nonce, plaintext, aad)

    rotate_pkt = Packet(type=PacketType.ROTATE_KEY, serial=disc.serial, epoch=old_epoch, nonce=nonce, payload=ciphertext)
    client.send(rotate_pkt, old_key)
    resp = client.recv()
    if resp.type != PacketType.ROTATE_KEY_RESP or not resp.verify_hmac(old_key):
        raise SystemExit("ROTATE_KEY failed (invalid response or wrong HMAC).")
    if status_byte(resp.payload) != AdminStatus.OK:
        raise SystemExit(f"ROTATE_KEY refused: {status_name(status_byte(resp.payload))}")
    print(f"{disc.serial}: new key staged on the device (epoch {target_epoch}), confirming...")

    confirm_pkt = Packet(type=PacketType.ROTATE_CONFIRM, serial=disc.serial, epoch=target_epoch, nonce=nonce, payload=b"")
    client.send(confirm_pkt, new_key)
    resp2 = client.recv()
    if resp2.type != PacketType.ROTATE_CONFIRM_RESP or not resp2.verify_hmac(new_key):
        raise SystemExit(
            "ROTATE_CONFIRM failed -- the new key may not have taken effect. "
            "Within 30s the device discards the staged key on its own and goes back to accepting only the old one."
        )
    if status_byte(resp2.payload) != AdminStatus.OK:
        raise SystemExit(f"ROTATE_CONFIRM refused: {status_name(status_byte(resp2.payload))}")
    print(f"{disc.serial}: rotation complete. New active epoch: {target_epoch}.")
    print(f"New key (re-derivable any time from MAC+epoch+master secret, no need to write it down): {new_key.hex()}")


def do_claim(ip: str, master_secret: bytes, operator: str,
             model: str = MODEL_PREFIX, log_path: Path = DEFAULT_PRODUCTION_LOG,
             port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> None:
    """Provisions a fresh, unprovisioned unit entirely over the network --
    no serial/debugger connection needed, no esptool involved. CLAIM is
    UNAUTHENTICATED, the same as DISCOVER -- there's nothing to
    authenticate against yet on a unit with no identity, and the device
    only ever accepts it while devid_is_provisioned() == false, so a
    stray CLAIM against an already-claimed unit is refused there, not
    here. Payload is plaintext: new_key[32] || new_epoch[4], no
    encryption envelope."""
    client = AdminClient(ip, port, timeout)
    disc = client.discover_one()
    info = parse_discover_payload(disc.payload)
    if info["provisioned"]:
        raise SystemExit(f"{disc.serial}: already provisioned -- CLAIM only works on fresh units (use rotate-key instead).")

    mac = mac_from_serial(disc.serial)
    new_key = derive_admin_key(master_secret, mac, 0, model)  # every unit's real identity starts at epoch 0

    claim_pkt = Packet(type=PacketType.CLAIM, serial=disc.serial, epoch=0, nonce=b"",
                        payload=new_key + struct.pack(">I", 0))
    client.send(claim_pkt, None)  # unauthenticated, same as DISCOVER
    resp = client.recv()
    if resp.type != PacketType.CLAIM_RESP:
        raise SystemExit("CLAIM failed (unexpected response type).")
    if status_byte(resp.payload) != AdminStatus.OK:
        raise SystemExit(f"CLAIM refused: {status_name(status_byte(resp.payload))}")

    append_production_log(log_path, disc.serial, mac, model, 0, info["fw_version"], operator)
    print(f"{disc.serial}: claimed successfully -- real admin key now active (epoch 0), no debugger involved.")
    print(f"Row added to {log_path} (without the key).")


def do_discover(broadcast: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> list[tuple[Packet, str]]:
    results = broadcast_discover(broadcast, port, timeout)
    if not results:
        print("No unit responded.")
        return results
    for pkt, src_ip in results:
        info = parse_discover_payload(pkt.payload)
        print(f"{pkt.serial}  ip={src_ip}  model={info['model']}  epoch={info['epoch']}  "
              f"fw={info['fw_version']}  provisioned={info['provisioned']}")
    return results


# ======================================================================= #
# Interactive menu -- scan first, pick a luminaire from the list, then get
# a device-specific menu with all its options (diskpart-style: "list" ->
# "select" -> act on the selected item). Commands are split into submenus
# by category (info & control vs. administration) instead of one long
# flat list.
# ======================================================================= #
def print_scan_results(results: list[tuple[Packet, str]]) -> None:
    if not results:
        print("No unit responded.")
        return
    for i, (pkt, src_ip) in enumerate(results, 1):
        info = parse_discover_payload(pkt.payload)
        tag = "provisioned" if info["provisioned"] else "UNPROVISIONED"
        print(f"  [{i}] {pkt.serial}  ip={src_ip}  epoch={info['epoch']}  "
              f"fw={info['fw_version']}  ({tag})")


def scan_devices() -> list[tuple[Packet, str]]:
    """Broadcasts DISCOVER and returns the responses, sorted by serial, for
    the top-level menu to number and let the operator select from."""
    default_bcast = guess_broadcast_address()
    broadcast = input(f"Broadcast address [{default_bcast}]: ").strip() or default_bcast
    print("Scanning...")
    try:
        results = broadcast_discover(broadcast, DEFAULT_PORT, DEFAULT_TIMEOUT)
    except OSError as e:
        print(f"Broadcast failed: {e}")
        return []
    results.sort(key=lambda r: r[0].serial)
    print_scan_results(results)
    return results


def resolve_device_by_ip(ip: str) -> tuple[Packet, dict] | None:
    """Unicasts a DISCOVER straight to a manually-typed IP (for units that
    didn't answer the broadcast, e.g. a different subnet) to fetch the
    serial/info the device menu header needs. None if it doesn't respond."""
    client = AdminClient(ip, DEFAULT_PORT, DEFAULT_TIMEOUT)
    try:
        disc = client.discover_one()
    except socket.timeout:
        return None
    return disc, parse_discover_payload(disc.payload)


def run_action(handler, ip: str) -> None:
    """Runs one action_* handler against the selected device, keeping the
    menu alive across the same errors main() used to guard against."""
    try:
        handler(ip)
    except socket.timeout:
        print("Timed out waiting for the unit's response -- right IP/port? Unit powered and provisioned?")
    except SystemExit as e:
        print(f"Error: {e}")
    except KeyboardInterrupt:
        print("\nCancelled.")
    except Exception as e:  # last resort -- keep the menu alive
        print(f"Unexpected error: {e}")


def run_action_noargs(handler) -> None:
    try:
        handler()
    except SystemExit as e:
        print(f"Error: {e}")
    except KeyboardInterrupt:
        print("\nCancelled.")
    except Exception as e:
        print(f"Unexpected error: {e}")


def action_provision(ip: str) -> None:
    operator = input("Operator name: ").strip()
    if not operator:
        print("Operator is required (goes in the production log), aborting.")
        return
    model = input(f"Model prefix [{MODEL_PREFIX}]: ").strip() or MODEL_PREFIX
    master_secret = get_master_secret(offer_choice=True)
    do_claim(ip, master_secret, operator, model)


def action_derive() -> None:
    mac = input("MAC (e.g. AA:BB:CC:DD:EE:FF): ").strip()
    epoch_raw = input("Epoch: ").strip()
    if not mac or not epoch_raw:
        print("MAC and epoch are both required.")
        return
    model = input(f"Model prefix [{MODEL_PREFIX}]: ").strip() or MODEL_PREFIX
    secret = get_master_secret(offer_choice=True)
    do_derive(mac, int(epoch_raw), model, secret)


def action_status(ip: str) -> None:
    do_status(ip, get_master_secret())


def action_identify(ip: str) -> None:
    do_identify(ip, get_master_secret())


def _prompt_ramp_ms() -> int:
    raw = input(f"Ramp time in ms [{DEFAULT_RAMP_MS}]: ").strip()
    return int(raw) if raw else DEFAULT_RAMP_MS


def action_on(ip: str) -> None:
    do_on(ip, get_master_secret(), _prompt_ramp_ms())


def action_off(ip: str) -> None:
    do_off(ip, get_master_secret(), _prompt_ramp_ms())


def action_dim(ip: str) -> None:
    percent_raw = input("Brightness percent (0-100): ").strip()
    if not percent_raw:
        print("Percent is required.")
        return
    do_dim(ip, get_master_secret(), int(percent_raw), _prompt_ramp_ms())


def action_reboot(ip: str) -> None:
    do_reboot(ip, get_master_secret())


def action_reset(ip: str) -> None:
    do_reset(ip, get_master_secret())


def action_rotate_key(ip: str) -> None:
    new_epoch_raw = input("New epoch (Enter for current+1): ").strip()
    do_rotate_key(ip, get_master_secret(), int(new_epoch_raw) if new_epoch_raw else None)


# (label, handler) -- handler takes the selected device's ip. Built down
# here, after the action_* functions above are all defined, since these
# dicts are evaluated at import time.
INFO_CONTROL_MENU = {
    "1": ("Status", action_status),
    "2": ("Identify (blink)", action_identify),
    "3": ("On", action_on),
    "4": ("Off", action_off),
    "5": ("Dim", action_dim),
}
ADMINISTRATION_MENU = {
    "1": ("Reboot", action_reboot),
    "2": ("Factory reset", action_reset),
    "3": ("Rotate admin key", action_rotate_key),
}


def run_submenu(title: str, items: dict[str, tuple[str, object]], ip: str) -> None:
    while True:
        print(f"\n  -- {title} --")
        for key, (label, _) in items.items():
            print(f"    {key}) {label}")
        print("    0) Back")
        choice = input("  > ").strip()
        if choice in ("0", ""):
            return
        if choice not in items:
            print("  Invalid choice.")
            continue
        _, handler = items[choice]
        run_action(handler, ip)


def device_menu(serial: str, ip: str, provisioned: bool) -> None:
    """The per-luminaire menu: everything you can do to one already-picked
    unit, grouped into submenus. Re-checks provisioned state after a
    successful CLAIM so the menu updates without having to reselect."""
    while True:
        print(f"\n=== {serial}  ip={ip} ===")
        if not provisioned:
            print("  Status: UNPROVISIONED")
            print("  1) Provision (CLAIM)")
            print("  0) Back to device list")
            choice = input("> ").strip()
            if choice in ("0", ""):
                return
            if choice == "1":
                run_action(action_provision, ip)
                resolved = resolve_device_by_ip(ip)
                if resolved is not None:
                    _, info = resolved
                    provisioned = info["provisioned"]
                continue
            print("Invalid choice.")
            continue

        print("  Status: provisioned")
        print("  1) Info & control  (status / identify / on / off / dim)")
        print("  2) Administration  (reboot / factory reset / rotate key)")
        print("  0) Back to device list")
        choice = input("> ").strip()
        if choice in ("0", ""):
            return
        if choice == "1":
            run_submenu("Info & control", INFO_CONTROL_MENU, ip)
        elif choice == "2":
            run_submenu("Administration", ADMINISTRATION_MENU, ip)
        else:
            print("Invalid choice.")


def main() -> None:
    print("=== DriverPoE admin tool ===")
    last_scan: list[tuple[Packet, str]] = []
    while True:
        print()
        if last_scan:
            print_scan_results(last_scan)
        else:
            print("(no scan yet)")
        print("  [S] Scan the network")
        print("  [M] Enter a device IP manually")
        print("  [K] Derive a key from MAC+epoch (recovery, no device needed)")
        print("  [Q] Quit")
        choice = input("> ").strip()
        lowered = choice.lower()

        if choice == "" or lowered == "q":
            break
        if lowered == "s":
            last_scan = scan_devices()
            continue
        if lowered == "k":
            run_action_noargs(action_derive)
            continue
        if lowered == "m":
            ip = input("IP: ").strip()
            if not ip:
                continue
            resolved = resolve_device_by_ip(ip)
            if resolved is None:
                print("No response from that IP.")
                continue
            disc, info = resolved
            device_menu(disc.serial, ip, info["provisioned"])
            continue

        try:
            idx = int(choice)
        except ValueError:
            print("Invalid choice.")
            continue
        if not last_scan or not (1 <= idx <= len(last_scan)):
            print("Invalid choice.")
            continue
        pkt, ip = last_scan[idx - 1]
        info = parse_discover_payload(pkt.payload)
        device_menu(pkt.serial, ip, info["provisioned"])


# ======================================================================= #
# Tests -- run with `python -m unittest lumtool -v` from inside tools/.
# In the same file on purpose (single .py file, no sibling test module):
# `-m unittest` imports this file as "lumtool", not "__main__", so the
# menu below never launches when running the tests, and `python
# lumtool.py` never runs the tests -- the two entry points don't collide.
#
# HKDF-SHA256 (RFC 5869 vectors + a "golden" vector for the project's
# actual scheme), packet serialization/deserialization, and HMAC
# computation. Standard-library unittest only.
# ======================================================================= #
import unittest


class TestHkdf(unittest.TestCase):
    def test_rfc5869_case1_sha256(self):
        # RFC 5869, Appendix A.1 (Test Case 1, Hash = SHA-256)
        ikm = bytes.fromhex("0b" * 22)
        salt = bytes.fromhex("000102030405060708090a0b0c")
        info = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9")
        length = 42
        expected_prk = bytes.fromhex(
            "077709362c2e32df0ddc3f0dc47bba63" "90b6c73bb50f9c3122ec844ad7c2b3e5"
        )
        expected_okm = bytes.fromhex(
            "3cb25f25faacd57a90434f64d0362f2a" "2d2d0a90cf1a5a4c5db02d56ecc4c5bf" "34007208d5b887185865"
        )

        prk = hkdf_extract(salt, ikm)
        self.assertEqual(prk, expected_prk)

        okm = hkdf_expand(prk, info, length)
        self.assertEqual(okm, expected_okm)

        self.assertEqual(hkdf_sha256(ikm, salt, info, length), expected_okm)

    def test_rfc5869_case3_sha256_zero_length_salt_info(self):
        # RFC 5869, Appendix A.3: empty salt and info, 22-byte IKM, L=42
        ikm = bytes.fromhex("0b" * 22)
        salt = b""
        info = b""
        length = 42
        expected_okm = bytes.fromhex(
            "8da4e775a563c18f715f802a063c5a31" "b8a11f5c5ee1879ec3454e5f3c738d2d" "9d201395faa4b61a96c8"
        )
        self.assertEqual(hkdf_sha256(ikm, salt, info, length), expected_okm)


class TestDeriveAdminKey(unittest.TestCase):
    def test_deterministic_and_length(self):
        secret = bytes.fromhex("00" * 32)
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        key1 = derive_admin_key(secret, mac, epoch=0)
        key2 = derive_admin_key(secret, mac, epoch=0)
        self.assertEqual(key1, key2)
        self.assertEqual(len(key1), KEY_LEN)

    def test_epoch_changes_key(self):
        secret = bytes.fromhex("11" * 32)
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        self.assertNotEqual(derive_admin_key(secret, mac, 0), derive_admin_key(secret, mac, 1))

    def test_mac_changes_key(self):
        secret = bytes.fromhex("22" * 32)
        key_a = derive_admin_key(secret, mac_from_str("AA:AA:AA:AA:AA:AA"), 0)
        key_b = derive_admin_key(secret, mac_from_str("BB:BB:BB:BB:BB:BB"), 0)
        self.assertNotEqual(key_a, key_b)

    def test_golden_vector(self):
        """Locks the derivation scheme in place: if this test breaks after
        a change to hkdf_*/derive_admin_key/build_hkdf_info, that change
        altered the key that already-flashed devices expect -- this isn't
        a flaky test, it's a real alarm."""
        secret = bytes.fromhex("00112233445566778899aabbccddeeff00112233445566778899aabbccddee")
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        key = derive_admin_key(secret, mac, epoch=0, model="LUM1")
        self.assertEqual(len(key), 32)
        self.assertEqual(key.hex(), "e3de1c690ab47f0ac3726d8d3686b6bce76f34add56dea7aa8786455256944ff")


class TestSerial(unittest.TestCase):
    def test_serial_format(self):
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        self.assertEqual(serial_from_mac(mac), "DriverPoE-A4CF12B93D08")

    def test_mac_roundtrip(self):
        for s in ("A4:CF:12:B9:3D:08", "a4-cf-12-b9-3d-08", "A4CF12B93D08"):
            self.assertEqual(mac_from_str(s), bytes.fromhex("A4CF12B93D08"))

    def test_mac_invalid(self):
        with self.assertRaises(ValueError):
            mac_from_str("not-a-mac")


class TestPacket(unittest.TestCase):
    def test_roundtrip_authenticated(self):
        key = bytes(range(32))
        pkt = Packet(
            type=PacketType.STATUS,
            serial="DriverPoE-A4CF12B93D08",
            epoch=3,
            nonce=bytes(range(16)),
            payload=b"hello",
        )
        wire = pkt.pack(hmac_key=key)
        self.assertEqual(len(wire), HEADER_SIZE + 5 + HMAC_LEN)

        parsed = Packet.unpack(wire)
        self.assertEqual(parsed.type, PacketType.STATUS)
        self.assertEqual(parsed.serial, "DriverPoE-A4CF12B93D08")
        self.assertEqual(parsed.epoch, 3)
        self.assertEqual(parsed.nonce, bytes(range(16)))
        self.assertEqual(parsed.payload, b"hello")
        self.assertTrue(parsed.verify_hmac(key))

    def test_roundtrip_unauthenticated_discover(self):
        pkt = Packet(type=PacketType.DISCOVER, serial="", epoch=0, nonce=b"", payload=b"")
        wire = pkt.pack(hmac_key=None)
        parsed = Packet.unpack(wire)
        self.assertEqual(parsed.type, PacketType.DISCOVER)
        self.assertEqual(parsed.payload, b"")

    def test_tampered_payload_fails_hmac(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.REBOOT, serial="DriverPoE-000000000000", epoch=0, nonce=b"\x01" * 16, payload=b"")
        wire = bytearray(pkt.pack(hmac_key=key))
        wire[HEADER_SIZE - 1] ^= 0xFF  # corrupt the last byte before the payload (payload_len)
        with self.assertRaises(ValueError):
            Packet.unpack(bytes(wire))

    def test_wrong_key_fails_verify(self):
        pkt = Packet(type=PacketType.STATUS, serial="DriverPoE-000000000000", epoch=0, nonce=b"", payload=b"x")
        wire = pkt.pack(hmac_key=bytes(range(32)))
        parsed = Packet.unpack(wire)
        self.assertFalse(parsed.verify_hmac(bytes(range(1, 33))))

    def test_short_packet_rejected(self):
        with self.assertRaises(ValueError):
            Packet.unpack(b"\x00" * 10)

    def test_bad_magic_rejected(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.STATUS, serial="", epoch=0, nonce=b"", payload=b"")
        wire = bytearray(pkt.pack(hmac_key=key))
        wire[0] ^= 0xFF
        with self.assertRaises(ValueError):
            Packet.unpack(bytes(wire))

    def test_truncated_length_field_rejected(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.STATUS, serial="", epoch=0, nonce=b"", payload=b"1234")
        wire = pkt.pack(hmac_key=key)
        with self.assertRaises(ValueError):
            Packet.unpack(wire[:-1])  # cut 1 byte -- the size no longer matches


class TestPacketTypes(unittest.TestCase):
    def test_claim_type_values(self):
        # Must stay in sync with components/admin_channel/admin_protocol.h's admin_pkt_type_t.
        self.assertEqual(PacketType.CLAIM, 0x09)
        self.assertEqual(PacketType.CLAIM_RESP, 0x89)

    def test_on_off_dim_type_values(self):
        # Must stay in sync with components/admin_channel/admin_protocol.h's admin_pkt_type_t.
        self.assertEqual(PacketType.ON, 0x0A)
        self.assertEqual(PacketType.ON_RESP, 0x8A)
        self.assertEqual(PacketType.OFF, 0x0B)
        self.assertEqual(PacketType.OFF_RESP, 0x8B)
        self.assertEqual(PacketType.DIM, 0x0C)
        self.assertEqual(PacketType.DIM_RESP, 0x8C)


class TestHmacVector(unittest.TestCase):
    def test_known_hmac_sha256_vector(self):
        # RFC 4231, Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?"
        key = b"Jefe"
        data = b"what do ya want for nothing?"
        expected = bytes.fromhex(
            "5bdcc146bf60754e6a042426089575c7" "5a003f089d2739839dec58b964ec3843"
        )
        self.assertEqual(hmac.new(key, data, hashlib.sha256).digest(), expected)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye.")
