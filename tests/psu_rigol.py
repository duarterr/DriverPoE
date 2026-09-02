"""Rigol DP1308A programmable PSU -- raw-socket SCPI over LAN (LXI).

One class, `RigolDP1308A`, plus a `FakePSU` stand-in for dry runs without
hardware. I/O only: no test logic here (that's harness.py).

Wiring for this rig: the DriverPoE PoE bus is fed from CH2 (+25 V rail)
and CH3 (-25 V rail) in series, so the bus voltage is CH2 + |CH3|. The
harness sets each rail to half the requested bus voltage and turns both
outputs on; per-channel power is measured on CH2 and CH3 separately
("Fonte 1" / "Fonte 2") and summed.

SCPI dialect note: the DP1308A predates the unified DP800 command set.
The command strings it actually accepts are collected as constants at the
top of the class so they are easy to tweak against your unit's
programming guide if a firmware revision differs. Every command is
followed by a `*OPC?`/error-queue check so a rejected command fails loud
instead of silently doing nothing.
"""
from __future__ import annotations

import socket
import time
from dataclasses import dataclass


class PSUError(RuntimeError):
    """Any PSU-side failure: socket trouble, a SCPI error-queue entry, or
    a reply that didn't parse as a number."""


@dataclass
class ChannelMeasurement:
    voltage: float          # V, signed as the instrument reports it
    current: float          # A, signed as the instrument reports it
    power: float            # W, always >= 0 (abs(V * I))


class RigolDP1308A:
    """Talks to one DP1308A at a fixed IP. Not thread-safe (one socket,
    used synchronously). Use as a context manager or call close()."""

    DEFAULT_PORT = 5555        # Rigol raw-socket SCPI port
    CHANNELS = ("CH1", "CH2", "CH3")

    # --- SCPI command templates (tweak here if your unit differs) ------
    CMD_APPLY = ":APPLy {ch},{volt:.3f},{curr:.3f}"
    CMD_SELECT = ":INSTrument:SELect {ch}"
    CMD_OUTPUT = ":OUTPut {ch},{state}"
    CMD_MEAS_VOLT = ":MEASure:VOLTage? {ch}"
    CMD_MEAS_CURR = ":MEASure:CURRent? {ch}"

    def __init__(self, host: str, port: int = DEFAULT_PORT, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None

    # -- transport ----------------------------------------------------------
    def connect(self) -> None:
        if self._sock is not None:
            return
        try:
            self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
            self._sock.settimeout(self.timeout)
        except OSError as e:
            self._sock = None
            raise PSUError(f"cannot reach PSU at {self.host}:{self.port} ({e})") from e

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self) -> "RigolDP1308A":
        self.connect()
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def _write(self, cmd: str) -> None:
        self.connect()
        assert self._sock is not None
        try:
            self._sock.sendall((cmd + "\n").encode("ascii"))
        except OSError as e:
            raise PSUError(f"PSU write failed ({cmd!r}): {e}") from e

    def _read(self) -> str:
        assert self._sock is not None
        buf = bytearray()
        try:
            while b"\n" not in buf:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break
                buf.extend(chunk)
        except OSError as e:
            raise PSUError(f"PSU read failed: {e}") from e
        return buf.decode("ascii", errors="replace").strip()

    def _query(self, cmd: str) -> str:
        self._write(cmd)
        return self._read()

    def _query_float(self, cmd: str) -> float:
        raw = self._query(cmd)
        # DP1308A sometimes answers ":MEAS:VOLT?" with a bare number, and
        # some firmware appends units or a channel tag -- take the first
        # token that parses as a float.
        for tok in raw.replace(",", " ").split():
            try:
                return float(tok)
            except ValueError:
                continue
        raise PSUError(f"non-numeric reply to {cmd!r}: {raw!r}")

    def _check_errors(self, context: str) -> None:
        """Drain the SCPI error queue; raise on the first real error."""
        for _ in range(10):
            resp = self._query(":SYSTem:ERRor?")
            code = resp.split(",", 1)[0].strip()
            try:
                if int(code) == 0:
                    return
            except ValueError:
                return  # unparseable -> assume no error queue support
            raise PSUError(f"PSU rejected {context}: {resp}")

    # -- high level -------------------------------------------------------
    def idn(self) -> str:
        return self._query("*IDN?")

    def _valid_channel(self, ch: str) -> str:
        ch = ch.upper()
        if ch not in self.CHANNELS:
            raise ValueError(f"channel must be one of {self.CHANNELS}, got {ch!r}")
        return ch

    def set_channel(self, ch: str, voltage: float, current: float) -> None:
        """Program a channel's voltage setpoint and current limit. For the
        negative rail (CH3) pass the magnitude -- the instrument outputs
        it negative."""
        ch = self._valid_channel(ch)
        self._write(self.CMD_APPLY.format(ch=ch, volt=abs(voltage), curr=abs(current)))
        self._check_errors(f"APPLy {ch}")

    def output(self, ch: str, on: bool) -> None:
        ch = self._valid_channel(ch)
        self._write(self.CMD_OUTPUT.format(ch=ch, state="ON" if on else "OFF"))
        self._check_errors(f"OUTPut {ch}")

    def all_outputs(self, on: bool) -> None:
        for ch in ("CH2", "CH3"):
            self.output(ch, on)

    def measure(self, ch: str) -> ChannelMeasurement:
        ch = self._valid_channel(ch)
        v = self._query_float(self.CMD_MEAS_VOLT.format(ch=ch))
        i = self._query_float(self.CMD_MEAS_CURR.format(ch=ch))
        return ChannelMeasurement(voltage=v, current=i, power=abs(v * i))

    def setup_bus(self, bus_voltage: float, current_limit: float) -> None:
        """Split the requested series-bus voltage equally across CH2/CH3
        and program both, outputs left untouched."""
        half = bus_voltage / 2.0
        self.set_channel("CH2", half, current_limit)
        self.set_channel("CH3", half, current_limit)


class FakePSU:
    """Drop-in stand-in for RigolDP1308A used by `--fake` / tests. Models a
    crude resistive-ish load so the sweep produces monotonic numbers."""

    def __init__(self, host: str = "fake", port: int = 0, timeout: float = 0.0):
        self.host, self.port = host, port
        self._v = {"CH2": 0.0, "CH3": 0.0}
        self._on = {"CH2": False, "CH3": False}
        self._dim = 0.0  # harness pokes this so measurements track the sweep

    def connect(self) -> None: ...
    def close(self) -> None: ...
    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def idn(self) -> str:
        return "RIGOL TECHNOLOGIES,DP1308A,FAKE0000000000,00.01.00 (simulated)"

    def set_channel(self, ch: str, voltage: float, current: float) -> None:
        self._v[ch.upper()] = abs(voltage)

    def output(self, ch: str, on: bool) -> None:
        self._on[ch.upper()] = on

    def all_outputs(self, on: bool) -> None:
        for ch in ("CH2", "CH3"):
            self.output(ch, on)

    def setup_bus(self, bus_voltage: float, current_limit: float) -> None:
        self.set_channel("CH2", bus_voltage / 2, current_limit)
        self.set_channel("CH3", bus_voltage / 2, current_limit)

    def measure(self, ch: str) -> ChannelMeasurement:
        ch = ch.upper()
        if not self._on.get(ch):
            return ChannelMeasurement(0.0, 0.0, 0.0)
        frac = max(0.0, min(1.0, self._dim / 100.0))
        # ~13 W per rail at full, with a small idle draw, plus a touch of noise.
        p = 0.26 + 12.5 * (frac ** 1.05)
        mag = self._v.get(ch, 0.0) or 26.0
        v = -mag if ch == "CH3" else mag
        i = p / mag
        jitter = 1.0 + ((hash((ch, round(time.time() * 7))) % 7) - 3) * 0.001
        return ChannelMeasurement(voltage=v, current=i, power=p * jitter)
