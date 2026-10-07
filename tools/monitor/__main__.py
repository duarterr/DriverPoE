"""Field monitor: polls one or more units' INFO (unauthenticated) on an
interval and logs every anomalous state transition -- PoE dropping out, a
reboot, the LED going dark while still commanded on, a power-budget
downgrade -- with the reset reason / PoE confirm bits / VBUS / LED voltage
/ budget needed to tell *why* apart after the fact.

Motivating symptom: a unit left ON goes dark on its own after a while, and
the PoE source visibly renegotiates around the same time. Comparing
consecutive INFO snapshots pins that down to one of a few distinct causes
that all look the same from "the LED turned off":

  - reboot        -- uptime_s went backwards; reset_reason says why the
                      MCU itself restarted (brownout, watchdog, panic...).
  - poe_lost       -- poe_ready flipped true->false with no reboot (VBUS
                      sagged below the operating threshold and came back,
                      or the source dropped/re-raised the class) --
                      seen at the board without the MCU itself resetting.
  - power_state_change -- the negotiated budget/cap changed (full -> capped
                      -> blocked -> no_power or back), independent of the
                      above two.
  - led_dark       -- driver_on went false while desired_on is still true
                      and neither of the above explains it by itself --
                      correlate with the other events logged at the same
                      timestamp.

Run from inside tools/:

    python -m monitor --board 192.168.1.61 --board 192.168.1.62
    python -m monitor --scan                          # discover + monitor every unit that answers
    python -m monitor --board 192.168.1.61 --log events.jsonl --interval 1

Ctrl+C stops it. Anomalies always print to stdout; with --log they are also
appended as JSON Lines (one object per event) for offline analysis. Pass
--snapshot-log to additionally record every successful poll (not just
anomalies) as JSONL -- useful to plot VBUS/uptime/power_state over time
after the fact.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, TextIO

from device_api import discovery
from device_api.client import AdminClient, DeviceTimeoutError
from device_api.models import DeviceInfo
from device_api.protocol import DEFAULT_PORT, DEFAULT_TIMEOUT, ProtocolError

# A unit that answers INFO at all is never in a worse state than this, in
# ascending order of "how bad" -- used to decide whether a power_state
# transition is a downgrade (warn) or a recovery (info).
_POWER_STATE_RANK = {"full": 0, "capped": 1, "blocked": 2, "no_power": 3}

DEFAULT_INTERVAL_S = 2.0
DEFAULT_VBUS_SAG_PCT = 10.0  # flag a VBUS drop of at least this much between two polls


# ======================================================================= #
# Anomaly model + pure diff logic -- no I/O, no sockets, unit-testable on
# its own (see tests/test_monitor.py) without touching real hardware.
# ======================================================================= #
@dataclass
class Anomaly:
    serial: str
    ip: str
    kind: str            # seen | unreachable | reachable | reboot | poe_lost |
    #                       poe_recovered | power_state_change | led_dark |
    #                       led_recovered | vbus_sag | fw_changed
    severity: str         # info | warn | crit
    message: str
    detail: dict[str, Any] = field(default_factory=dict)


def diff_snapshots(serial: str, ip: str, prev: DeviceInfo, cur: DeviceInfo,
                    vbus_sag_pct: float = DEFAULT_VBUS_SAG_PCT) -> list[Anomaly]:
    """Compares two consecutive successful INFO reads from the same unit
    and returns every anomaly the transition explains. Order matters for
    readability (reboot first -- it explains most of what follows) but not
    for correctness; multiple anomalies from one transition are normal and
    meant to be read together."""
    events: list[Anomaly] = []

    if cur.uptime_s < prev.uptime_s:
        events.append(Anomaly(
            serial, ip, "reboot", "crit",
            f"{serial}: rebooted (uptime {prev.uptime_s}s -> {cur.uptime_s}s), "
            f"reset_reason={cur.reset_reason}",
            {"prev_uptime_s": prev.uptime_s, "uptime_s": cur.uptime_s, "reset_reason": cur.reset_reason},
        ))

    if prev.poe_ready and not cur.poe_ready:
        events.append(Anomaly(
            serial, ip, "poe_lost", "crit",
            f"{serial}: PoE dropped out ({cur.power_blocking_reason}); "
            f"VBUS {prev.vbus_mv}mV -> {cur.vbus_mv}mV, source was {prev.poe_source}",
            {"prev_vbus_mv": prev.vbus_mv, "vbus_mv": cur.vbus_mv, "poe_source": cur.poe_source,
             "cdb_confirmed": cur.poe_cdb_confirmed, "t2p_confirmed": cur.poe_t2p_confirmed,
             "vbus_confirmed": cur.poe_vbus_confirmed},
        ))
    elif not prev.poe_ready and cur.poe_ready:
        events.append(Anomaly(
            serial, ip, "poe_recovered", "info",
            f"{serial}: PoE re-negotiated and is ready again "
            f"(source={cur.poe_source}, VBUS={cur.vbus_mv}mV)",
        ))

    if cur.power_state_name != prev.power_state_name:
        worse = _POWER_STATE_RANK.get(cur.power_state_name, 0) > _POWER_STATE_RANK.get(prev.power_state_name, 0)
        events.append(Anomaly(
            serial, ip, "power_state_change", "warn" if worse else "info",
            f"{serial}: power state {prev.power_state_name} -> {cur.power_state_name} "
            f"(budget {prev.power_budget_w:g}W -> {cur.power_budget_w:g}W, "
            f"LD scale {cur.power_effective_scale_pct}%)",
            {"prev_power_state": prev.power_state_name, "power_state": cur.power_state_name,
             "prev_budget_w": prev.power_budget_w, "budget_w": cur.power_budget_w},
        ))

    if cur.desired_on and prev.driver_on and not cur.driver_on:
        events.append(Anomaly(
            serial, ip, "led_dark", "crit",
            f"{serial}: LED turned OFF on its own while still commanded ON "
            f"(was dimmed to {prev.dim_percent}%); power_state={cur.power_state_name}, "
            f"poe_ready={cur.poe_ready}",
            {"prev_dim_percent": prev.dim_percent, "power_state": cur.power_state_name,
             "poe_ready": cur.poe_ready},
        ))
    elif cur.desired_on and not prev.driver_on and cur.driver_on:
        events.append(Anomaly(
            serial, ip, "led_recovered", "info",
            f"{serial}: LED is back ON (dim={cur.dim_percent}%)",
        ))

    if prev.vbus_mv > 0 and cur.vbus_mv > 0:
        drop_pct = (prev.vbus_mv - cur.vbus_mv) / prev.vbus_mv * 100.0
        if drop_pct >= vbus_sag_pct:
            tail = " without losing poe_ready" if cur.poe_ready else ""
            events.append(Anomaly(
                serial, ip, "vbus_sag", "warn",
                f"{serial}: VBUS sagged {drop_pct:.0f}% ({prev.vbus_mv}mV -> {cur.vbus_mv}mV){tail}",
                {"prev_vbus_mv": prev.vbus_mv, "vbus_mv": cur.vbus_mv, "drop_pct": round(drop_pct, 1)},
            ))

    if cur.fw_version != prev.fw_version:
        events.append(Anomaly(
            serial, ip, "fw_changed", "info",
            f"{serial}: firmware {prev.fw_version} -> {cur.fw_version}",
        ))

    return events


class DeviceState:
    """Tracks one unit's rolling state across polls -- the last good INFO,
    and whether it's currently flagged unreachable. `observe()` is pure
    (no I/O of its own) so the whole state machine is unit-testable by
    feeding it a scripted sequence of DeviceInfo/None."""

    def __init__(self, ip: str, vbus_sag_pct: float = DEFAULT_VBUS_SAG_PCT) -> None:
        self.ip = ip
        self.serial = ip
        self.last_info: DeviceInfo | None = None
        self.unreachable_since: float | None = None
        self._vbus_sag_pct = vbus_sag_pct

    def observe(self, cur: DeviceInfo | None, now: float) -> list[Anomaly]:
        """`cur` is None for a poll that timed out / failed to parse.
        `now` is a monotonic timestamp, only used to size the outage
        report on recovery."""
        if cur is None:
            if self.unreachable_since is None:
                self.unreachable_since = now
                return [Anomaly(self.serial, self.ip, "unreachable", "crit",
                                 f"{self.serial} ({self.ip}): stopped answering INFO")]
            return []

        events: list[Anomaly] = []
        if self.unreachable_since is not None:
            gap = now - self.unreachable_since
            events.append(Anomaly(cur.serial, self.ip, "reachable", "info",
                                   f"{cur.serial} ({self.ip}): answering again after {gap:.1f}s unreachable"))
            self.unreachable_since = None

        if self.last_info is None:
            events.append(Anomaly(
                cur.serial, self.ip, "seen", "info",
                f"{cur.serial} ({self.ip}): first seen -- fw={cur.fw_version} "
                f"poe_ready={cur.poe_ready} power_state={cur.power_state_name} "
                f"dim={cur.dim_percent}% uptime={cur.uptime_s}s",
            ))
        else:
            events.extend(diff_snapshots(cur.serial, self.ip, self.last_info, cur, self._vbus_sag_pct))

        self.serial = cur.serial
        self.last_info = cur
        return events


# ======================================================================= #
# I/O: polling + printing/logging -- everything above this line is pure.
# ======================================================================= #
def poll_once(client: AdminClient) -> DeviceInfo | None:
    try:
        return client.info()
    except (DeviceTimeoutError, ProtocolError):
        return None


_SEVERITY_TAG = {"info": "  ", "warn": "! ", "crit": "!!"}


def emit(ev: Anomaly, log_fh: TextIO | None) -> None:
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"{stamp} {_SEVERITY_TAG.get(ev.severity, '? ')} [{ev.kind}] {ev.message}", flush=True)
    if log_fh is not None:
        row = {"time": stamp, "serial": ev.serial, "ip": ev.ip, "kind": ev.kind,
               "severity": ev.severity, "message": ev.message, **ev.detail}
        log_fh.write(json.dumps(row, ensure_ascii=False) + "\n")
        log_fh.flush()


def log_snapshot(info: DeviceInfo, fh: TextIO) -> None:
    row = {
        "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "serial": info.serial, "ip": info.source_ip, "fw_version": info.fw_version,
        "uptime_s": info.uptime_s, "reset_reason": info.reset_reason,
        "poe_ready": info.poe_ready, "poe_source": info.poe_source, "vbus_mv": info.vbus_mv,
        "driver_on": info.driver_on, "desired_on": info.desired_on, "dim_percent": info.dim_percent,
        "led_voltage_mv": info.led_voltage_mv, "power_state": info.power_state_name,
        "power_budget_w": info.power_budget_w, "power_effective_scale_pct": info.power_effective_scale_pct,
    }
    fh.write(json.dumps(row, ensure_ascii=False) + "\n")
    fh.flush()


def discover_boards(broadcast: str | None, iface: str | None, port: int, timeout: float) -> list[str]:
    """Broadcasts an INFO and returns every unit that answers. `iface`, if
    given, is a local IPv4 (see list_local_ipv4s()) to bind the socket to
    and, absent an explicit `broadcast`, to derive the target /24 from --
    the fix for a multi-homed host where guess_broadcast_address() picked
    the wrong NIC (or none at all reach the units' LAN)."""
    bcast = broadcast or (discovery.broadcast_address_for(iface) if iface else discovery.guess_broadcast_address())
    print(f"Scanning {bcast}:{port}" + (f" via {iface}" if iface else "") + " ...")
    found = discovery.broadcast_info(bcast, port, timeout, bind_ip=iface)
    for info in sorted(found, key=lambda i: i.serial):
        print(f"  found {info.serial}  ip={info.source_ip}  fw={info.fw_version}")
    return [info.source_ip for info in found]


def print_interfaces() -> None:
    addrs = discovery.list_local_ipv4s()
    if not addrs:
        print("Couldn't enumerate local interfaces -- check with ipconfig/ip addr "
              "and pass --broadcast (and optionally --iface) explicitly.")
        return
    print("Local IPv4 addresses (pass one as --iface to scan out that NIC):")
    for ip in addrs:
        print(f"  {ip}   (broadcast {discovery.broadcast_address_for(ip)})")


def run(args: argparse.Namespace) -> int:
    boards = list(dict.fromkeys(args.board))  # de-dup, keep order
    if args.scan or not boards:
        boards = list(dict.fromkeys(
            boards + discover_boards(args.broadcast, args.iface, args.port, args.timeout)))
    if not boards:
        print("No units to monitor -- give --board IP (repeatable) or --scan "
              "(--list-interfaces if the scan finds nothing on a multi-homed host).", file=sys.stderr)
        return 1

    states = {ip: DeviceState(ip, args.vbus_sag_pct) for ip in boards}
    clients = {ip: AdminClient(ip, args.port, args.timeout) for ip in boards}

    log_fh = open(args.log, "a", encoding="utf-8") if args.log else None
    snap_fh = open(args.snapshot_log, "a", encoding="utf-8") if args.snapshot_log else None

    print(f"Monitoring {len(boards)} unit(s) every {args.interval:g}s -- Ctrl+C to stop.")
    for ip in boards:
        print(f"  {ip}")

    try:
        while True:
            tick_start = time.monotonic()
            for ip, client in clients.items():
                cur = poll_once(client)
                if cur is not None and snap_fh is not None:
                    log_snapshot(cur, snap_fh)
                for ev in states[ip].observe(cur, time.monotonic()):
                    if args.quiet and ev.severity == "info":
                        continue
                    emit(ev, log_fh)
            elapsed = time.monotonic() - tick_start
            time.sleep(max(0.0, args.interval - elapsed))
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        for client in clients.values():
            client.close()
        if log_fh is not None:
            log_fh.close()
        if snap_fh is not None:
            snap_fh.close()
    return 0


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="monitor", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--board", action="append", default=[], metavar="IP",
                    help="unit IP to monitor (repeatable)")
    ap.add_argument("--scan", action="store_true",
                    help="broadcast-discover units and monitor every one that answers "
                         "(merged with --board; used automatically if no --board is given)")
    ap.add_argument("--broadcast", help="broadcast address for --scan (default: auto-guessed, "
                                         "or derived from --iface if given)")
    ap.add_argument("--iface", metavar="LOCAL_IP",
                     help="local IPv4 to scan out of (see --list-interfaces) -- binds the scan "
                          "socket to it and, unless --broadcast is also given, derives the "
                          "broadcast address from its /24. Needed on a multi-homed host where "
                          "the auto-guessed interface isn't the one wired to the units.")
    ap.add_argument("--list-interfaces", action="store_true",
                     help="print this host's local IPv4 addresses (one per NIC) and exit")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=1.5,
                    help="per-poll socket timeout, seconds (default 1.5)")
    ap.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_S,
                    help="seconds between polls of the same unit (default %(default)s)")
    ap.add_argument("--vbus-sag-pct", type=float, default=DEFAULT_VBUS_SAG_PCT,
                    help="flag a VBUS drop of at least this %% between two polls (default %(default)s)")
    ap.add_argument("--log", metavar="PATH", help="append anomalies as JSON Lines to this file")
    ap.add_argument("--snapshot-log", metavar="PATH",
                    help="append every successful poll (not just anomalies) as JSON Lines")
    ap.add_argument("--quiet", action="store_true", help="only print warn/crit anomalies, not info-level ones")
    return ap.parse_args(argv)


def main() -> None:
    args = _parse_args()
    if args.list_interfaces:
        print_interfaces()
        sys.exit(0)
    if args.log:
        Path(args.log).parent.mkdir(parents=True, exist_ok=True)
    if args.snapshot_log:
        Path(args.snapshot_log).parent.mkdir(parents=True, exist_ok=True)
    sys.exit(run(args))


if __name__ == "__main__":
    main()
