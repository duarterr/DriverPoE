"""A minimal Art-Net (ArtDmx) transmitter for the demo control surface.

The demo no longer drives fixtures over the authenticated admin channel:
each fixture is commissioned once in the admin UI (universe + DMX start
address + personality), and from then on this sender streams ArtDmx to
them like any lighting console -- no HMAC, no per-command nonce, no rate
limit. That's exactly what the firmware's DMX input layer
(components/dmx_input/) is for.

One background thread transmits every patched universe at a fixed rate,
whether or not anything changed, so the fixtures always see a live source
and never fall back to their signal-loss behavior mid-show. Stop the
sender (or the whole app) and, after each fixture's loss timeout, the
admin channel regains control.
"""
from __future__ import annotations

import socket
import struct
import threading
import time

ARTNET_PORT = 6454
DEFAULT_FPS = 40  # just under the DMX512 / Art-Net ceiling of ~44

_ARTNET_ID = b"Art-Net\x00"
_OP_DMX = 0x5000
_PROT_VER = 14


def build_artdmx(sequence: int, port_address: int, slots: bytes) -> bytes:
    """One ArtDmx packet. port_address is the 15-bit Net:SubNet:Universe."""
    data = bytes(slots)
    if len(data) % 2:
        data += b"\x00"
    if len(data) < 2:
        data = data.ljust(2, b"\x00")
    sub_uni = port_address & 0xFF
    net = (port_address >> 8) & 0x7F
    hdr = _ARTNET_ID + struct.pack("<H", _OP_DMX) + bytes(
        [0, _PROT_VER, sequence & 0xFF, 0, sub_uni, net]
    ) + struct.pack(">H", len(data))
    return hdr + data


class ArtNetSender:
    """Holds one 512-slot buffer per patched universe and unicasts each to
    the fixtures on it, continuously, from a background thread."""

    def __init__(self, fps: int = DEFAULT_FPS):
        self._fps = fps
        self._lock = threading.Lock()
        self._buffers: dict[int, bytearray] = {}       # port_address -> 512 slots
        self._dests: dict[int, list[str]] = {}         # port_address -> [fixture ip]
        self._seq: dict[int, int] = {}
        self._patch: dict[str, tuple[int, int, int]] = {}  # ip -> (port_address, addr_1based, channels)
        self._levels: dict[str, int] = {}              # ip -> last percent we set
        self._strobe: dict | None = None               # {picks, period, on, frame} -- driven by the TX loop
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()

    # -- patch -------------------------------------------------------------
    def set_patch(self, fixtures: list[dict]) -> None:
        """fixtures: dicts with ip, port_address, address (1-based), channels (1 or 2)."""
        with self._lock:
            self._buffers.clear()
            self._dests.clear()
            self._patch.clear()
            for f in fixtures:
                pa = f["port_address"] & 0x7FFF
                self._patch[f["ip"]] = (pa, f["address"], f.get("channels", 1))
                self._buffers.setdefault(pa, bytearray(512))
                self._dests.setdefault(pa, []).append(f["ip"])
                self._seq.setdefault(pa, 0)
            # keep only levels for still-patched fixtures
            self._levels = {ip: self._levels.get(ip, 0) for ip in self._patch}
            self._apply_all_locked()

    # -- level control ---------------------------------------------------
    def set_level(self, ip: str, percent: int) -> None:
        percent = max(0, min(100, int(percent)))
        with self._lock:
            if ip not in self._patch:
                return
            self._levels[ip] = percent
            self._apply_one_locked(ip, percent)

    def set_many(self, items: list[tuple[str, int]]) -> None:
        with self._lock:
            for ip, percent in items:
                if ip in self._patch:
                    percent = max(0, min(100, int(percent)))
                    self._levels[ip] = percent
                    self._apply_one_locked(ip, percent)

    def all_levels(self) -> dict[str, int]:
        with self._lock:
            return dict(self._levels)

    # -- strobe --------------------------------------------------------------
    def set_strobe(self, ips, hz: float, duty: float) -> None:
        """Blink `ips` on/off from inside the TX loop, so the pulses are
        locked to the 40 Hz frame clock instead of a separate sleep loop
        that beats against it. The rate is quantised to whole frames:
        period = round(fps / hz) frames, on = round(period * duty%)."""
        period = max(2, round(self._fps / max(0.1, hz)))
        on = min(period - 1, max(1, round(period * duty / 100.0)))
        picks = {ip for ip in ips if ip in self._patch}
        with self._lock:
            st = self._strobe
            if st and st["picks"] == picks and st["period"] == period and st["on"] == on:
                return
            if st:                                  # fixtures dropped from the strobe
                for ip in st["picks"] - picks:      # go straight back to their steady level
                    self._apply_one_locked(ip, self._levels.get(ip, 0))
            self._strobe = {"picks": picks, "period": period, "on": on,
                            "frame": (st["frame"] % period) if st else 0}

    def clear_strobe(self) -> None:
        with self._lock:
            st = self._strobe
            self._strobe = None
            if st:
                for ip in st["picks"]:
                    self._apply_one_locked(ip, self._levels.get(ip, 0))

    def strobe_actual_hz(self, hz: float) -> float:
        return self._fps / max(2, round(self._fps / max(0.1, hz)))

    def _apply_one_locked(self, ip: str, percent: int) -> None:
        pa, addr, channels = self._patch[ip]
        buf = self._buffers.get(pa)
        if buf is None:
            return
        value16 = round(percent * 65535 / 100)
        if channels >= 2:
            buf[addr - 1] = (value16 >> 8) & 0xFF
            if addr < 512:
                buf[addr] = value16 & 0xFF
        else:
            buf[addr - 1] = round(percent * 255 / 100)

    def _apply_all_locked(self) -> None:
        for ip, percent in self._levels.items():
            self._apply_one_locked(ip, percent)

    # -- transmit loop --------------------------------------------------
    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="artnet-tx", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        t = self._thread
        if t:
            t.join(timeout=1.0)
        self._thread = None

    @property
    def running(self) -> bool:
        return bool(self._thread and self._thread.is_alive())

    def _run(self) -> None:
        interval = 1.0 / self._fps
        next_tick = time.monotonic()
        while not self._stop.is_set():
            with self._lock:
                st = self._strobe
                if st and st["picks"]:
                    st["frame"] = (st["frame"] + 1) % st["period"]
                    lit = 100 if st["frame"] < st["on"] else 0
                    for ip in st["picks"]:
                        self._apply_one_locked(ip, lit)
                frames = []
                for pa, buf in self._buffers.items():
                    self._seq[pa] = (self._seq.get(pa, 0) % 255) + 1
                    pkt = build_artdmx(self._seq[pa], pa, bytes(buf))
                    for ip in self._dests.get(pa, []):
                        frames.append((pkt, ip))
            for pkt, ip in frames:
                try:
                    self._sock.sendto(pkt, (ip, ARTNET_PORT))
                except OSError:
                    pass
            # absolute schedule: frame N leaves at t0 + N*interval, so the
            # strobe (counted in frames) stays locked to the wall clock and
            # a slow send() doesn't push the next frame late.
            next_tick += interval
            slack = next_tick - time.monotonic()
            if slack < -interval:            # fell way behind -- resync
                next_tick = time.monotonic()
            elif slack > 0:
                time.sleep(slack)
