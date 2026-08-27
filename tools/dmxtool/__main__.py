"""Art-Net / sACN test transmitter.

Examples (run from inside tools/):

    # Art-Net: hold channel 1 of universe 0 at 50% on the local broadcast
    python -m dmxtool artnet --ip 192.168.1.255 --universe 0 --channel 1 --value 128

    # Art-Net: 0->255->0 triangle ramp, 5 s period, for 30 s
    python -m dmxtool artnet --ip 192.168.1.255 --universe 0 --channel 1 --ramp --period 5 --duration 30

    # sACN: same ramp, sent to the standard multicast group for universe 1
    python -m dmxtool sacn --universe 1 --channel 1 --ramp

    # sACN unicast to a specific device
    python -m dmxtool sacn --ip 192.168.1.42 --universe 1 --channel 1 --value 200

The transmitter sends ~30 frames/s. Ctrl+C stops it (sACN sends a final
stream_terminated packet on the way out).
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import time

ARTNET_PORT = 6454
SACN_PORT = 5568
FPS = 30


def _level_at(t: float, args) -> int:
    if not args.ramp:
        return args.value
    # symmetric triangle 0..255..0 over `period` seconds
    phase = (t % args.period) / args.period
    tri = phase * 2 if phase < 0.5 else (1 - phase) * 2
    return round(tri * 255)


def _dmx_slots(args, level: int) -> bytes:
    n = max(args.channel, args.channels)
    buf = bytearray(n)
    buf[args.channel - 1] = level & 0xFF
    if args.channels >= 2 and args.channel < n:
        # 16-bit personality: put the fine byte in the next slot
        buf[args.channel] = 0
    return bytes(buf)


# --------------------------------------------------------------------- #
# Art-Net
# --------------------------------------------------------------------- #
def artnet_packet(seq: int, port_address: int, slots: bytes) -> bytes:
    sub_uni = port_address & 0xFF
    net = (port_address >> 8) & 0x7F
    length = len(slots)
    if length % 2:
        slots += b"\x00"
        length += 1
    hdr = b"Art-Net\x00" + struct.pack("<H", 0x5000) + bytes([0, 14, seq & 0xFF, 0, sub_uni, net])
    hdr += struct.pack(">H", length)
    return hdr + slots


def run_artnet(args) -> None:
    port_address = ((args.net & 0x7F) << 8) | ((args.subnet & 0x0F) << 4) | (args.universe & 0x0F)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    dst = (args.ip, ARTNET_PORT)
    print(f"Art-Net -> {args.ip}:{ARTNET_PORT}  port-address 0x{port_address:04x} "
          f"(net {args.net}/sub {args.subnet}/uni {args.universe})  channel {args.channel}")
    _loop(lambda t, seq: sock.sendto(artnet_packet(seq, port_address, _dmx_slots(args, _level_at(t, args))), dst), args)


# --------------------------------------------------------------------- #
# sACN / E1.31
# --------------------------------------------------------------------- #
def sacn_packet(cid: bytes, seq: int, universe: int, slots: bytes, priority: int, terminated: bool) -> bytes:
    slot_data = b"\x00" + slots  # start code + slots
    dmp_len = 10 + len(slot_data)
    frame_len = 77 + dmp_len
    root_len = 22 + frame_len

    root = struct.pack(">HH", 0x0010, 0x0000)
    root += b"ASC-E1.17\x00\x00\x00"
    root += struct.pack(">H", 0x7000 | root_len)
    root += struct.pack(">I", 0x00000004)
    root += cid

    options = 0x40 if terminated else 0x00
    frame = struct.pack(">H", 0x7000 | frame_len)
    frame += struct.pack(">I", 0x00000002)
    frame += b"dmxtool".ljust(64, b"\x00")
    frame += bytes([priority])
    frame += struct.pack(">H", 0)          # sync address
    frame += bytes([seq & 0xFF, options])
    frame += struct.pack(">H", universe)

    dmp = struct.pack(">H", 0x7000 | dmp_len)
    dmp += bytes([0x02, 0xA1])
    dmp += struct.pack(">HH", 0x0000, 0x0001)
    dmp += struct.pack(">H", len(slot_data))
    dmp += slot_data
    return root + frame + dmp


def _sacn_mcast(universe: int) -> str:
    return f"239.255.{(universe >> 8) & 0xFF}.{universe & 0xFF}"


def run_sacn(args) -> None:
    cid = os.urandom(16)
    dst_ip = args.ip or _sacn_mcast(args.universe)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    dst = (dst_ip, SACN_PORT)
    print(f"sACN -> {dst_ip}:{SACN_PORT}  universe {args.universe}  priority {args.priority}  channel {args.channel}")

    def send(t: float, seq: int) -> None:
        sock.sendto(sacn_packet(cid, seq, args.universe, _dmx_slots(args, _level_at(t, args)),
                                args.priority, terminated=False), dst)

    try:
        _loop(send, args)
    finally:
        # three stream_terminated packets, per E1.31 recommendation
        for k in range(3):
            sock.sendto(sacn_packet(cid, 250 + k, args.universe, _dmx_slots(args, 0),
                                    args.priority, terminated=True), dst)
        print("sent stream_terminated")


# --------------------------------------------------------------------- #
def _loop(send, args) -> None:
    seq = 0
    start = time.monotonic()
    interval = 1.0 / FPS
    try:
        while True:
            now = time.monotonic()
            t = now - start
            if args.duration and t >= args.duration:
                break
            send(t, seq)
            seq = (seq + 1) & 0xFF
            time.sleep(max(0.0, interval - (time.monotonic() - now)))
    except KeyboardInterrupt:
        print()


def main() -> None:
    p = argparse.ArgumentParser(prog="dmxtool", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="proto", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--ip", help="destination IP (broadcast/unicast for Art-Net; unicast for sACN, default = the universe multicast group)")
    common.add_argument("--universe", type=int, default=0, help="universe number")
    common.add_argument("--channel", type=int, default=1, help="1-based DMX start channel to drive (default 1)")
    common.add_argument("--channels", type=int, default=1, help="how many channels the fixture occupies (1 or 2)")
    common.add_argument("--value", type=int, default=255, help="fixed 0-255 level (ignored with --ramp)")
    common.add_argument("--ramp", action="store_true", help="send a 0->255->0 triangle instead of a fixed level")
    common.add_argument("--period", type=float, default=4.0, help="ramp period in seconds")
    common.add_argument("--duration", type=float, default=0.0, help="stop after N seconds (0 = run until Ctrl+C)")

    a = sub.add_parser("artnet", parents=[common], help="send ArtDmx")
    a.add_argument("--net", type=int, default=0, help="Art-Net Net (0-127)")
    a.add_argument("--subnet", type=int, default=0, help="Art-Net Sub-Net (0-15)")
    a.set_defaults(func=run_artnet)

    s = sub.add_parser("sacn", parents=[common], help="send E1.31 data packets")
    s.add_argument("--priority", type=int, default=100, help="sACN priority 0-200")
    s.set_defaults(func=run_sacn)

    args = p.parse_args()
    if args.proto == "artnet" and not args.ip:
        p.error("Art-Net needs --ip (a broadcast or unicast address)")
    args.func(args)


if __name__ == "__main__":
    main()
