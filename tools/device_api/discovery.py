"""Finding devices on the network -- broadcast INFO, or resolve one known
IP directly. No authentication involved (INFO is the one unauthenticated
command) -- see client.py for everything past this point.
"""
from __future__ import annotations

import socket
import time

from .models import DeviceInfo
from .protocol import DEFAULT_PORT, DEFAULT_TIMEOUT, Packet, PacketType, ProtocolError, parse_info_payload

FALLBACK_BROADCAST = "255.255.255.255"  # used only if the guess below fails


def guess_broadcast_address() -> str:
    """Best-effort guess of this machine's local broadcast address, from
    whatever interface the OS would pick to reach the internet -- assumes
    a /24 subnet (true for the overwhelming majority of the small LANs
    this tool runs on). A caller-facing default to offer/override, not
    something to trust blindly."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(0.5)
            s.connect(("8.8.8.8", 80))  # UDP "connect" -- no packet actually sent
            local_ip = s.getsockname()[0]
        return ".".join(local_ip.split(".")[:3]) + ".255"
    except OSError:
        return FALLBACK_BROADCAST


def broadcast_info(bcast_ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> list[DeviceInfo]:
    """Sends one INFO to the broadcast address and collects every
    INFO_RESP that arrives within `timeout`. Never raises for "nobody
    answered" -- an empty list is a normal, common outcome (a network scan
    with zero devices reachable from here). Malformed responses (wrong
    protocol version, corrupt packet) are silently skipped -- a scan
    should be resilient to junk on the wire (or a straggler from a
    different protocol version) rather than abort the whole scan; a
    per-unit version mismatch shows up instead when something later talks
    to that specific device directly (client.py's calls raise
    ProtocolVersionMismatchError there)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    pkt = Packet(type=PacketType.INFO, serial="", nonce=b"", payload=b"")
    sock.sendto(pkt.pack(None), (bcast_ip, port))

    results: list[DeviceInfo] = []
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
        except ConnectionResetError:
            # Windows-only quirk (see client.py's _recv() comment): an
            # ICMP port-unreachable from one non-responding host on the
            # broadcast domain can surface as WSAECONNRESET here instead
            # of just being ignored -- must not abort the whole scan over
            # one host that isn't there; keep waiting out the remaining
            # deadline for everyone else's replies.
            continue
        try:
            resp = Packet.unpack(data)
        except ProtocolError:
            continue
        if resp.type != PacketType.INFO_RESP:
            continue
        try:
            fields = parse_info_payload(resp.payload)
        except ProtocolError:
            continue
        results.append(DeviceInfo(serial=resp.serial, source_ip=src[0], **fields))
    sock.close()
    return results


def resolve_device_by_ip(ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> DeviceInfo | None:
    """Unicasts an INFO straight to a manually-known IP (for units that
    didn't answer a broadcast, e.g. a different subnet/VLAN). None if it
    doesn't respond in time -- "not found" is a normal outcome here too,
    not an exception; a caller that wants an exception instead can raise
    client.DeviceNotFoundError itself around a None result."""
    from .client import AdminClient, DeviceTimeoutError  # local import: avoids a client<->discovery cycle

    with AdminClient(ip, port, timeout) as client:
        try:
            return client.info()
        except (DeviceTimeoutError, ProtocolError):
            return None
