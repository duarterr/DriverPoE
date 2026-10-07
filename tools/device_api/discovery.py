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


def broadcast_address_for(local_ip: str) -> str:
    """The /24 broadcast address for a given local IPv4 -- true for the
    overwhelming majority of the small LANs this tool runs on. Shared by
    guess_broadcast_address() and any caller that already knows which
    local interface it wants (see list_local_ipv4s())."""
    return ".".join(local_ip.split(".")[:3]) + ".255"


def guess_broadcast_address() -> str:
    """Best-effort guess of this machine's local broadcast address, from
    whatever interface the OS would pick to reach the internet. On a
    machine with more than one NIC (Ethernet + Wi-Fi, a VPN adapter...)
    this is only ever a guess -- if the units don't answer, list the real
    candidates with list_local_ipv4s() and pass the right one's broadcast
    address (or bind to it directly) explicitly instead."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(0.5)
            s.connect(("8.8.8.8", 80))  # UDP "connect" -- no packet actually sent
            local_ip = s.getsockname()[0]
        return broadcast_address_for(local_ip)
    except OSError:
        return FALLBACK_BROADCAST


def list_local_ipv4s() -> list[str]:
    """Every local IPv4 address this host answers to, loopback excluded --
    one per NIC in the common case (Ethernet, Wi-Fi, a VPN adapter...).
    Stdlib-only (no netifaces/psutil): asks the resolver for every address
    behind this machine's own hostname, which is what every OS this tool
    targets (Windows included) populates from the active adapters. Best
    effort -- an adapter with no DNS-visible binding can be missing; when
    in doubt, `ipconfig`/`ip addr` remains the ground truth. Used to let a
    caller pick which interface a broadcast scan should go out of, on a
    multi-homed machine where guess_broadcast_address() may pick wrong."""
    try:
        _hostname, _aliases, addrs = socket.gethostbyname_ex(socket.gethostname())
    except socket.gaierror:
        return []
    return sorted({a for a in addrs if not a.startswith("127.")})


def broadcast_info(bcast_ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT,
                    bind_ip: str | None = None) -> list[DeviceInfo]:
    """Sends one INFO to the broadcast address and collects every
    INFO_RESP that arrives within `timeout`. Never raises for "nobody
    answered" -- an empty list is a normal, common outcome (a network scan
    with zero devices reachable from here). Malformed responses (wrong
    protocol version, corrupt packet) are silently skipped -- a scan
    should be resilient to junk on the wire (or a straggler from a
    different protocol version) rather than abort the whole scan; a
    per-unit version mismatch shows up instead when something later talks
    to that specific device directly (client.py's calls raise
    ProtocolVersionMismatchError there).

    `bind_ip`, if given, pins the socket to that local address (one of
    list_local_ipv4s()) before sending -- on a multi-homed host this is
    what actually forces the broadcast out a specific NIC; `bcast_ip`
    alone only reliably does that when its /24 matches just one adapter's
    subnet. Raises OSError if `bind_ip` isn't a local address."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        if bind_ip is not None:
            sock.bind((bind_ip, 0))
        sock.settimeout(timeout)
        pkt = Packet(type=PacketType.INFO, serial="", nonce=b"", payload=b"")
        sock.sendto(pkt.pack(None), (bcast_ip, port))
    except OSError:
        sock.close()
        raise

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
