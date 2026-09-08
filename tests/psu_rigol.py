"""RIGOL DP1308A control through the instrument's original Web UI protocol.

The DP1308A Web Control page does NOT expose a raw SCPI socket.  Its
JavaScript (DP1000_WebControl.html) drives the instrument by POSTing
numeric front-panel key codes to a single endpoint:

    http://<host>/lxi/infomation.xml

and parsing the XML that comes back for measurements and status.  The
browser sends the key code as the raw POST body (e.g. body == "4097");
Firefox instead does a GET with the code in the query string
(``?4097&timeStamp=<ms>``).  Some firmware revisions only answer one of
the two, so this client tries POST first and falls back to GET.

Two things about the embedded server bite a naive client:

  * the XML is declared ``encoding="gb2312"``, which Python's expat parser
    rejects with "unknown encoding" -- we decode the bytes ourselves and
    drop the ``<?xml ... ?>`` prolog before parsing;
  * the server is picky about HTTP; we keep the request as close to the
    browser's as possible (no Content-Type, just Cache-Control).

Only the Python standard library is used -- no NI-VISA / PyVISA / vendor
software.

The public API matches the previous psu_rigol.py:
    connect(), close(), idn(), set_channel(), output(), all_outputs(),
    measure(), setup_bus()

Channel names used by the harness:
    CH1 -> P6V     CH2 -> P25V     CH3 -> N25V

Run ``python -m tests.psu_rigol <host>`` for a connectivity probe that
dumps the raw XML and the decoded per-channel readings.
"""

from __future__ import annotations

import re
import socket
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass


class PSUError(RuntimeError):
    """Any PSU-side HTTP/protocol/XML failure."""


@dataclass
class ChannelMeasurement:
    voltage: float
    current: float
    power: float


class RigolDP1308A:
    """Control a DP1308A through its legacy HTTP Web Control protocol."""

    DEFAULT_PORT = 80
    CHANNELS = ("CH1", "CH2", "CH3")

    # Channel-selection keys (the "P6V" / "P25V" / "N25V" hard keys).
    CMD_SELECT = {
        "CH1": "4129",   # P6V
        "CH2": "4097",   # P25V
        "CH3": "4113",   # N25V
    }

    # Per-channel output on/off keys (toggle -- state must be read first).
    CMD_OUTPUT = {
        "CH1": "8273",
        "CH2": "8241",
        "CH3": "8257",
    }

    CMD_ALL_ON = "8209"
    CMD_ALL_OFF = "8225"

    # Refresh key: exactly what the page polls with when the panel is not
    # in "remote" (unlocked) mode.  Returns the full status XML.
    CMD_REFRESH = "0"

    # Soft keys F1..F5 along the bottom of the Web UI.
    CMD_F1 = "12289"   # Volt entry
    CMD_F2 = "12305"   # Curr entry
    CMD_F3 = "12321"
    CMD_F4 = "12337"
    CMD_F5 = "12353"

    CMD_OK = "8305"
    CMD_CANCEL = "16561"

    # ALL ON / ALL OFF (and sometimes a single-channel ON) make the panel
    # pop an "are you sure?" prompt that must be acknowledged.  These keys
    # are tried, in order, until the <MES> prompt clears: knob OK first,
    # then the F1 soft key.
    CMD_CONFIRM = ("8305", "12289")
    CONFIRM_TRIES = 6

    # The DP1308A's HTTP server is tiny and drops connections when hit
    # back-to-back ("Remote end closed connection without response").
    # Space requests out and retry transient failures.
    CMD_GAP_S = 0.20
    _TRANSIENT = (
        "closed connection", "connection reset", "reset by peer",
        "timed out", "timeout", "remote end closed", "bad status line",
        "not enough data", "incompleteread", "connectionreseterror",
    )

    # Numeric keypad.
    CMD_NUM = {
        "0": "16385",
        "1": "16401",
        "2": "16417",
        "3": "16433",
        "4": "16449",
        "5": "16465",
        "6": "16481",
        "7": "16497",
        "8": "16513",
        "9": "16529",
        ".": "16545",
    }

    # XML tag prefixes per channel.  `<{prefix}V/A/W>` are the *measured*
    # output values; `<{prefix}INV>` / `<{prefix}INA>` are the programmed
    # voltage setpoint / current limit; `<{prefix}STAT>` is ON/OFF.
    _PREFIX = {"CH1": "P6", "CH2": "P25", "CH3": "N25"}
    _STAT = {"CH1": "P6STAT", "CH2": "P25STAT", "CH3": "N25STAT"}

    # Channels wired to the PoE bus (CH2 = +rail, CH3 = -rail).
    BUS_CHANNELS = ("CH2", "CH3")

    # A setpoint within this of target counts as "already set" -- skip the
    # keypad dance (and its confirm prompts) entirely.
    SET_TOL_V = 0.05
    SET_TOL_A = 0.02

    def __init__(
        self,
        host: str,
        port: int = DEFAULT_PORT,
        timeout: float = 5.0,
    ):
        # Accept a bare IP/host or a full "http://host[:port]/..." URL.
        host = host.strip()
        if "://" in host:
            _, rest = host.split("://", 1)
            host = rest.split("/", 1)[0]
        else:
            host = host.split("/", 1)[0]
        if host.count(":") == 1 and host.rsplit(":", 1)[1].isdigit():
            host, url_port = host.rsplit(":", 1)
            if port == self.DEFAULT_PORT:
                port = int(url_port)

        self.host = host
        self.port = int(port)
        self.timeout = timeout
        self.base_url = f"http://{self.host}:{self.port}"
        self.endpoint = self.base_url + "/lxi/infomation.xml"
        self._connected = False
        self._method = "POST"          # locked to whatever answered first
        self._last_ts = 0.0            # monotonic time of the last request
        self.last_response: bytes = b""  # raw bytes of the last reply (debug)

    # ------------------------------------------------------------------ #
    # HTTP transport -- mirrors the page's callServer()
    # ------------------------------------------------------------------ #

    def connect(self) -> None:
        """Probe the Web Control endpoint and lock in a working transport."""
        if self._connected:
            return
        payload = self._transport(self.CMD_REFRESH, learn=True, idempotent=True)
        # Make sure it is actually the status page and not, say, a login
        # redirect or a 404 body that happened to be 200.
        self._xml_root(payload)
        self._connected = True

    def close(self) -> None:
        # HTTP is stateless; nothing to tear down.
        self._connected = False

    def __enter__(self) -> "RigolDP1308A":
        self.connect()
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def _pace(self) -> None:
        """Don't fire requests faster than CMD_GAP_S apart."""
        gap = self.CMD_GAP_S - (time.monotonic() - self._last_ts)
        if gap > 0:
            time.sleep(gap)

    def _http(self, command: str, method: str, *, tries: int = 1) -> bytes:
        """One HTTP round trip (mirrors the page's callServer()).

        `tries` > 1 retries transient connection drops -- only safe for
        idempotent commands (the status refresh), never a key press.
        """
        if method == "GET":
            ts = str(int(time.time() * 1000))
            url = f"{self.endpoint}?{command}&timeStamp={ts}"
            body = None
        else:
            url = self.endpoint
            body = str(command).encode("ascii")

        last: Exception | None = None
        for attempt in range(max(1, tries)):
            self._pace()
            req = urllib.request.Request(url, data=body, method=method)
            req.add_header("Cache-Control", "no-cache")
            req.add_header("Connection", "close")
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as response:
                    data = response.read()
                self._last_ts = time.monotonic()
                return data
            except urllib.error.HTTPError as e:
                self._last_ts = time.monotonic()
                raise PSUError(
                    f"PSU HTTP {e.code} for command {command!r} ({method}): {e.reason}"
                ) from e
            except (urllib.error.URLError, OSError, socket.error) as e:
                self._last_ts = time.monotonic()
                last = e
                reason = str(getattr(e, "reason", e))
                transient = any(h in reason.lower() for h in self._TRANSIENT)
                if not transient or attempt == tries - 1:
                    break
                time.sleep(0.4 * (attempt + 1))

        raise PSUError(
            f"cannot reach PSU at {self.endpoint} "
            f"(command {command!r}, {method}): {getattr(last, 'reason', last)}"
        ) from last

    def _transport(
        self, command: str, *, learn: bool = False, idempotent: bool = False
    ) -> bytes:
        """Send `command`, returning the raw reply.

        Tries the current transport; if it errors or comes back empty and
        `learn` is set, tries the other HTTP method and sticks with it.
        `idempotent` commands (the status refresh) also get retried on a
        transient connection drop.
        """
        order = [self._method]
        if learn:
            order.append("GET" if self._method == "POST" else "POST")
        tries = 3 if idempotent else 1

        first_error: PSUError | None = None
        for method in order:
            try:
                payload = self._http(command, method, tries=tries)
            except PSUError as e:
                first_error = first_error or e
                continue
            if payload.strip():
                if learn and method != self._method:
                    self._method = method
                self.last_response = payload
                return payload
            first_error = first_error or PSUError(
                f"empty response from PSU for command {command!r} ({method})"
            )

        raise first_error or PSUError(f"no response from PSU for {command!r}")

    def _command(self, command: str, *, idempotent: bool = False) -> bytes:
        if not self._connected:
            self.connect()
        return self._transport(command, idempotent=idempotent)

    # ------------------------------------------------------------------ #
    # XML helpers
    # ------------------------------------------------------------------ #

    @staticmethod
    def _decode(payload: bytes) -> str:
        for enc in ("utf-8", "gb18030", "gb2312", "latin-1"):
            try:
                return payload.decode(enc)
            except UnicodeDecodeError:
                continue
        return payload.decode("latin-1", errors="replace")

    @classmethod
    def _xml_root(cls, payload: bytes) -> ET.Element:
        text = cls._decode(payload).lstrip("﻿").strip()
        # expat chokes on `encoding="gb2312"`; strip the prolog and parse
        # the already-decoded text.
        text = re.sub(r"^\s*<\?xml[^>]*\?>\s*", "", text, count=1)
        if not text:
            raise PSUError("empty response body from PSU Web UI")
        try:
            return ET.fromstring(text)
        except ET.ParseError as e:
            raise PSUError(f"invalid XML from PSU: {text[:200]!r}") from e

    @staticmethod
    def _xml_value(root: ET.Element, tag: str) -> str:
        node = root.find(f".//{tag}")
        if node is None or node.text is None:
            raise PSUError(f"PSU XML response has no <{tag}> element")
        return node.text.strip()

    @classmethod
    def _xml_float(cls, root: ET.Element, tag: str) -> float:
        raw = cls._xml_value(root, tag)
        # Values arrive like "12.700 V" / "0.000 A" / "-20.7 V".
        m = re.search(r"[-+]?\d*\.?\d+", raw.replace(",", "."))
        if not m:
            raise PSUError(f"non-numeric <{tag}> value from PSU: {raw!r}")
        return float(m.group(0))

    def _status(self) -> ET.Element:
        return self._xml_root(self._command(self.CMD_REFRESH, idempotent=True))

    def _clear_prompt(self) -> None:
        """Acknowledge the panel's 'are you sure?' prompt, if one is up.

        ALL ON / ALL OFF (and sometimes a single-channel ON) raise a
        confirmation that shows up as a populated <MES> element.  Try the
        confirm keys until it clears.
        """
        for i in range(self.CONFIRM_TRIES):
            node = self._status().find(".//MES")
            if node is None or not (node.text or "").strip():
                return
            key = self.CMD_CONFIRM[min(i, len(self.CMD_CONFIRM) - 1)]
            self._transport(key)
            time.sleep(0.25)

    # ------------------------------------------------------------------ #
    # Public operations
    # ------------------------------------------------------------------ #

    def idn(self) -> str:
        # The legacy XML page has no *IDN? equivalent -- confirm it answers
        # and return a fixed identifier.
        self._status()
        return "RIGOL TECHNOLOGIES,DP1308A,WEB-CONTROL,DP1308A"

    def _valid_channel(self, ch: str) -> str:
        ch = ch.upper()
        if ch not in self.CHANNELS:
            raise ValueError(
                f"channel must be one of {self.CHANNELS}, got {ch!r}"
            )
        return ch

    def _select(self, ch: str) -> None:
        self._command(self.CMD_SELECT[self._valid_channel(ch)])

    def _enter_value(self, value: float) -> None:
        """Type a decimal value on the Web UI keypad and confirm with OK."""
        if value < 0:
            raise ValueError("DP1308A keypad entry cannot be negative")

        text = f"{value:.3f}".rstrip("0").rstrip(".") or "0"
        for char in text:
            try:
                self._command(self.CMD_NUM[char])
            except KeyError as e:
                raise ValueError(f"cannot key in value {value!r}") from e
        self._command(self.CMD_OK)

    def _set_parameter(self, ch: str, parameter: str, value: float) -> None:
        self._select(ch)
        if parameter == "volt":
            self._command(self.CMD_F1)
        elif parameter == "curr":
            self._command(self.CMD_F2)
        else:
            raise ValueError(parameter)
        self._enter_value(value)

    def setpoint(self, ch: str) -> tuple[float, float]:
        """The channel's *programmed* (voltage setpoint, current limit)."""
        ch = self._valid_channel(ch)
        root = self._status()
        p = self._PREFIX[ch]
        return abs(self._xml_float(root, p + "INV")), abs(self._xml_float(root, p + "INA"))

    def is_on(self, ch: str) -> bool:
        ch = self._valid_channel(ch)
        return self._xml_value(self._status(), self._STAT[ch]).upper() == "ON"

    def set_channel(
        self, ch: str, voltage: float, current: float, *, force: bool = False
    ) -> bool:
        """Program voltage setpoint and current limit via the keypad.

        Reads the current setpoint first and only keys in the parameters
        that are actually off-target (unless `force`).  Returns True if it
        touched the panel.
        """
        ch = self._valid_channel(ch)
        v_t, i_t = abs(voltage), abs(current)

        need_v = need_i = True
        if not force:
            cur_v, cur_i = self.setpoint(ch)
            need_v = abs(cur_v - v_t) > self.SET_TOL_V
            need_i = abs(cur_i - i_t) > self.SET_TOL_A

        if need_v:
            self._set_parameter(ch, "volt", v_t)
        if need_i:
            self._set_parameter(ch, "curr", i_t)
        return need_v or need_i

    def output(self, ch: str, on: bool) -> None:
        """Toggle a channel only if its current state differs from `on`."""
        ch = self._valid_channel(ch)
        state = self._xml_value(self._status(), self._STAT[ch]).upper()
        if state != ("ON" if on else "OFF"):
            self._command(self.CMD_OUTPUT[ch])
            self._clear_prompt()

    def all_outputs(self, on: bool) -> None:
        """Use the Web UI's ALL ON / ALL OFF keys (acknowledging the prompt)."""
        self._command(self.CMD_ALL_ON if on else self.CMD_ALL_OFF)
        self._clear_prompt()

    def measure(self, ch: str) -> ChannelMeasurement:
        """Read the values the Web UI shows for one channel."""
        ch = self._valid_channel(ch)
        root = self._status()
        prefix = self._PREFIX[ch]

        voltage = self._xml_float(root, prefix + "V")
        current = self._xml_float(root, prefix + "A")
        power = self._xml_float(root, prefix + "W")

        # N25V is shown negative in the UI but the XML carries the magnitude.
        if ch == "CH3" and voltage > 0:
            voltage = -voltage

        return ChannelMeasurement(
            voltage=voltage,
            current=abs(current),
            power=abs(power),
        )

    def setup_bus(self, bus_voltage: float, current_limit: float) -> list[str]:
        """Split the series bus equally across the two bus rails.

        Only reprograms a rail whose setpoint is off-target.  Returns the
        list of channels it actually keyed in.
        """
        half = bus_voltage / 2.0
        return [
            ch for ch in self.BUS_CHANNELS
            if self.set_channel(ch, half, current_limit)
        ]

    def bus_outputs_on(self) -> bool:
        root = self._status()
        return all(
            self._xml_value(root, self._STAT[ch]).upper() == "ON"
            for ch in self.BUS_CHANNELS
        )

    def ensure_bus_on(self, *, verify_timeout: float = 6.0) -> bool:
        """Make sure both bus rails are ON -- the board only talks to the
        network when it is powered.  Issues only what's needed and then
        verifies.  Returns True if it had to switch anything on.
        """
        root = self._status()
        off = [
            ch for ch in self.BUS_CHANNELS
            if self._xml_value(root, self._STAT[ch]).upper() != "ON"
        ]
        if not off:
            return False

        if len(off) == len(self.BUS_CHANNELS):
            self.all_outputs(True)
        else:
            for ch in off:
                self.output(ch, True)

        deadline = time.monotonic() + verify_timeout
        while time.monotonic() < deadline:
            if self.bus_outputs_on():
                return True
            time.sleep(0.4)
        raise PSUError(
            "PSU bus outputs did not turn ON "
            "(unacknowledged confirm prompt on the panel?)"
        )


class FakePSU:
    """Drop-in stand-in used by --fake/tests."""

    def __init__(self, host: str = "fake", port: int = 0, timeout: float = 0.0):
        self.host, self.port = host, port
        self._v = {"CH2": 0.0, "CH3": 0.0}
        self._on = {"CH2": False, "CH3": False}
        self._dim = 0.0

    def connect(self) -> None:
        pass

    def close(self) -> None:
        pass

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *a):
        self.close()

    def idn(self) -> str:
        return "RIGOL TECHNOLOGIES,DP1308A,FAKE0000000000,00.01.00 (simulated)"

    def set_channel(self, ch: str, voltage: float, current: float,
                    *, force: bool = False) -> bool:
        ch = ch.upper()
        changed = force or abs(self._v.get(ch, 0.0) - abs(voltage)) > 0.05
        self._v[ch] = abs(voltage)
        return changed

    def setpoint(self, ch: str) -> tuple[float, float]:
        return self._v.get(ch.upper(), 0.0), 1.0

    def is_on(self, ch: str) -> bool:
        return bool(self._on.get(ch.upper()))

    def output(self, ch: str, on: bool) -> None:
        self._on[ch.upper()] = on

    def all_outputs(self, on: bool) -> None:
        for ch in ("CH2", "CH3"):
            self.output(ch, on)

    def bus_outputs_on(self) -> bool:
        return all(self._on.get(ch) for ch in ("CH2", "CH3"))

    def ensure_bus_on(self, *, verify_timeout: float = 6.0) -> bool:
        was = self.bus_outputs_on()
        self.all_outputs(True)
        return not was

    def setup_bus(self, bus_voltage: float, current_limit: float) -> list[str]:
        return [
            ch for ch in ("CH2", "CH3")
            if self.set_channel(ch, bus_voltage / 2, current_limit)
        ]

    def measure(self, ch: str) -> ChannelMeasurement:
        ch = ch.upper()
        if not self._on.get(ch):
            return ChannelMeasurement(0.0, 0.0, 0.0)

        frac = max(0.0, min(1.0, self._dim / 100.0))
        p = 0.26 + 12.5 * (frac ** 1.05)
        mag = self._v.get(ch, 0.0) or 26.0
        v = -mag if ch == "CH3" else mag
        i = p / mag
        jitter = 1.0 + ((hash((ch, round(time.time() * 7))) % 7) - 3) * 0.001

        return ChannelMeasurement(
            voltage=v,
            current=i,
            power=p * jitter,
        )


# ---------------------------------------------------------------------- #
# Connectivity probe:  python -m tests.psu_rigol <host> [port]
# ---------------------------------------------------------------------- #

def _probe(argv: list[str]) -> int:
    if not argv:
        print("usage: python -m tests.psu_rigol <host> [port]", file=sys.stderr)
        return 2

    host = argv[0]
    port = int(argv[1]) if len(argv) > 1 else RigolDP1308A.DEFAULT_PORT
    psu = RigolDP1308A(host, port)

    print(f"endpoint : {psu.endpoint}")
    try:
        psu.connect()
    except PSUError as e:
        print(f"FAILED   : {e}")
        if psu.last_response:
            print("--- raw response (first 500 bytes) ---")
            print(psu.last_response[:500])
        return 1

    print(f"transport: {psu._method}")
    print("--- raw XML ---")
    print(psu._decode(psu.last_response))
    print("--- decoded readings ---")
    for ch in RigolDP1308A.CHANNELS:
        try:
            m = psu.measure(ch)
            print(f"  {ch}: {m.voltage:+.3f} V  {m.current:.3f} A  {m.power:.3f} W")
        except PSUError as e:
            print(f"  {ch}: {e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_probe(sys.argv[1:]))
