"""Automated dimming/power sweep: drive a DriverPoE board through a list
of dimming levels while a Rigol DP1308A feeds its PoE bus, and record the
mean power drawn on each supply rail at every level.

Flow per run (one or more boards, swept one after another):

  1. Apply the requested driver config (dimming mode + params +
     linearization) to every selected board.
  2. Program the PSU: CH2 and CH3 each to half the requested bus voltage,
     both outputs ON. The board only powers up once the source is on.
  3. Wait for each board to report PoE power, send ON.
  4. For each dimming level: DIM -> wait `settle_s` -> take `samples`
     paired V/I readings on CH2 and CH3 `sample_interval_s` apart ->
     record the mean per-rail power (mean of V*I per sample) and the
     total.
  5. Teardown: DIM 0 + OFF on the board; optionally turn the source off.

This module is I/O + orchestration only; the web UI (server.py) owns all
HTTP and the shared run state. It can also be run standalone -- see
`python -m tests.harness --help`.
"""
from __future__ import annotations

import argparse
import csv
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field, asdict
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

# device_api lives under tools/ -- make it importable without installing.
# Also put this dir on the path so `psu_rigol` resolves however we're run
# (python harness.py, python -m harness, python -m tests.harness).
_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent / "tools"
for _p in (str(_HERE), str(_TOOLS)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from device_api import (  # noqa: E402
    AdminClient,
    AuthError,
    DriverConfig,
    DriverPoEError,
    KeyfileSecretStore,
    MemorySecretStore,
    find_working_secret,
)
from device_api.protocol import DRIVER_MODE_NAMES  # noqa: E402

from psu_rigol import FakePSU, PSUError, RigolDP1308A  # noqa: E402

RESULTS_DIR = Path(__file__).resolve().parent / "results"

DEFAULT_LEVELS = [0, 1, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

# Presets matching the reference measurement table (label -> driver params).
PRESETS: dict[str, dict[str, int]] = {
    "Hibrido 20%": dict(mode=2, pwm_freq_hz=2000, analog_freq_hz=60000, min_on_time_us=20, crossover_pct=20),
    "Adim 60k":    dict(mode=1, pwm_freq_hz=2000, analog_freq_hz=60000, min_on_time_us=20, crossover_pct=20),
    "Pdim 2k, 2us": dict(mode=0, pwm_freq_hz=2000, analog_freq_hz=60000, min_on_time_us=2, crossover_pct=20),
}


# ======================================================================= #
# Config
# ======================================================================= #
@dataclass
class BoardRef:
    ip: str
    serial: str = ""


@dataclass
class SweepConfig:
    boards: list[BoardRef]

    # PSU
    psu_host: str = ""
    psu_port: int = RigolDP1308A.DEFAULT_PORT
    bus_voltage: float = 52.0
    current_limit_a: float = 1.0
    source_off_when_done: bool = True

    # Driver config to apply before the sweep
    label: str = ""                 # free-text run label (e.g. "Hibrido 20%")
    mode: int = 2                   # 0 pwm, 1 analog, 2 hybrid
    pwm_freq_hz: int = 2000
    analog_freq_hz: int = 60000
    min_on_time_us: int = 20
    crossover_pct: int = 20
    power_mode: int = 0            # 0 auto, 1 poe_only, 2 poe_plus_required
    poe_cap_pct: int = 51
    lin_enable: bool = True

    # Sweep
    levels: list[int] = field(default_factory=lambda: list(DEFAULT_LEVELS))
    settle_s: float = 1.0
    samples: int = 10
    sample_interval_s: float = 0.2
    power_timeout_s: float = 20.0

    fake: bool = False             # use FakePSU + skip real board I/O errors softly

    def driver_config(self) -> DriverConfig:
        return DriverConfig(
            mode=self.mode,
            pwm_freq_hz=self.pwm_freq_hz,
            analog_freq_hz=self.analog_freq_hz,
            min_on_time_us=self.min_on_time_us,
            crossover_pct=self.crossover_pct,
            power_mode=self.power_mode,
            poe_cap_pct=self.poe_cap_pct,
            lin_enable=1 if self.lin_enable else 0,
        )


@dataclass
class Row:
    board_serial: str
    board_ip: str
    fw_version: str
    label: str
    mode_name: str
    lin_enable: bool
    bus_voltage: float
    dimming_pct: int
    f1_w: float          # mean CH2 power
    f2_w: float          # mean CH3 power
    total_w: float
    f1_v: float
    f1_a: float
    f2_v: float
    f2_a: float
    samples: int
    timestamp: str


CSV_FIELDS = list(Row.__annotations__.keys())


# ======================================================================= #
# Runner
# ======================================================================= #
class SweepRunner:
    """Owns one sweep run on a background thread and the state the UI
    polls. Single run at a time -- start() is a no-op while one is live."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.state: str = "idle"          # idle | running | done | stopped | error
        self.error: str | None = None
        self.log: list[str] = []
        self.rows: list[dict[str, Any]] = []
        self.total_points: int = 0
        self.done_points: int = 0
        self.current: dict[str, Any] = {}
        self.csv_path: str | None = None
        self.started_at: str | None = None
        self.config: dict[str, Any] | None = None

    # -- lifecycle ------------------------------------------------------
    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "state": self.state,
                "error": self.error,
                "log": list(self.log[-200:]),
                "rows": list(self.rows),
                "total_points": self.total_points,
                "done_points": self.done_points,
                "current": dict(self.current),
                "csv_path": self.csv_path,
                "started_at": self.started_at,
                "config": self.config,
            }

    def stop(self) -> None:
        self._stop.set()

    def start(self, cfg: SweepConfig, store) -> bool:
        if self.running:
            return False
        self._stop.clear()
        with self._lock:
            self.state = "running"
            self.error = None
            self.log = []
            self.rows = []
            self.total_points = len(cfg.boards) * len(cfg.levels)
            self.done_points = 0
            self.current = {}
            self.csv_path = None
            self.started_at = datetime.now().isoformat(timespec="seconds")
            self.config = {**asdict(cfg), "boards": [asdict(b) for b in cfg.boards]}
        self._thread = threading.Thread(target=self._run, args=(cfg, store), daemon=True)
        self._thread.start()
        return True

    # -- internals ----------------------------------------------------------
    def _emit(self, msg: str) -> None:
        line = f"{datetime.now().strftime('%H:%M:%S')}  {msg}"
        with self._lock:
            self.log.append(line)

    def _add_row(self, row: Row) -> None:
        with self._lock:
            self.rows.append(asdict(row))
            self.done_points += 1

    def _set_current(self, **kw: Any) -> None:
        with self._lock:
            self.current = kw

    def _run(self, cfg: SweepConfig, store) -> None:
        psu: Any = None
        try:
            psu = FakePSU() if cfg.fake else RigolDP1308A(cfg.psu_host, cfg.psu_port)
            psu.connect()
            self._emit(f"PSU: {psu.idn()}")

            self._emit(
                f"Programming bus {cfg.bus_voltage:.1f} V "
                f"(CH2/CH3 = {cfg.bus_voltage / 2:.2f} V, {cfg.current_limit_a:.2f} A limit)"
            )
            psu.setup_bus(cfg.bus_voltage, cfg.current_limit_a)
            psu.all_outputs(True)
            self._emit("Source outputs ON")

            for board in cfg.boards:
                if self._stop.is_set():
                    break
                self._sweep_board(cfg, store, psu, board)

            if self._stop.is_set():
                self.state = "stopped"
                self._emit("Run stopped by operator")
            else:
                self.state = "done"
                self._emit("Run complete")
        except (PSUError, DriverPoEError, AuthError, OSError, ValueError) as e:
            self.state = "error"
            self.error = str(e)
            self._emit(f"ERROR: {e}")
        except Exception as e:  # noqa: BLE001  -- surface anything to the UI
            self.state = "error"
            self.error = f"{type(e).__name__}: {e}"
            self._emit(f"ERROR: {self.error}")
        finally:
            if psu is not None:
                try:
                    if cfg.source_off_when_done or self.state in ("error",):
                        psu.all_outputs(False)
                        self._emit("Source outputs OFF")
                except Exception as e:  # noqa: BLE001
                    self._emit(f"warning: could not turn source off ({e})")
                try:
                    psu.close()
                except Exception:  # noqa: BLE001
                    pass
            self._write_csv()

    def _resolve_secret(self, client: AdminClient, serial: str, store) -> bytes:
        secret, _ = find_working_secret(client, serial, store)
        return secret

    def _sweep_board(self, cfg: SweepConfig, store, psu: Any, board: BoardRef) -> None:
        self._emit(f"--- Board {board.serial or board.ip} @ {board.ip} ---")
        if cfg.fake:
            self._sweep_board_fake(cfg, psu, board)
            return
        with AdminClient(board.ip) as client:
            info = client.info()
            serial = info.serial
            secret = self._resolve_secret(client, serial, store)

            # 1. apply driver config
            cfgblob = cfg.driver_config()
            res = client.set_driver_config(secret, serial, cfgblob)
            if not res.accepted:
                raise DriverPoEError(f"{serial}: DRIVER_SET_CONFIG refused ({res.status.name})")
            applied = client.get_driver_config(secret, serial)
            self._emit(
                f"{serial}: mode={DRIVER_MODE_NAMES.get(applied.mode, applied.mode)} "
                f"pwm={applied.pwm_freq_hz}Hz analog={applied.analog_freq_hz}Hz "
                f"min_on={applied.min_on_time_us}us xover={applied.crossover_pct}% "
                f"lin={'on' if applied.lin_enable else 'off'}"
            )

            # 2. wait for PoE power, then ON
            deadline = time.monotonic() + cfg.power_timeout_s
            while True:
                info = client.info()
                if info.poe_ready or info.power_budget_w > 0:
                    break
                if time.monotonic() > deadline:
                    raise DriverPoEError(
                        f"{serial}: no PoE power after {cfg.power_timeout_s:.0f}s "
                        f"(reason: {info.power_blocking_reason or 'unknown'})"
                    )
                if self._stop.is_set():
                    return
                time.sleep(0.5)
            self._emit(f"{serial}: powered ({info.power_budget_w:.2f} W budget, {info.poe_source})")
            client.on(secret, serial, ramp_ms=0)

            # 3. sweep
            mode_name = DRIVER_MODE_NAMES.get(applied.mode, str(applied.mode))
            for pct in cfg.levels:
                if self._stop.is_set():
                    break
                client.dim(secret, serial, pct, ramp_ms=0)
                self._set_current(board=serial, dimming_pct=pct,
                                  done=self.done_points, total=self.total_points)
                time.sleep(cfg.settle_s)
                row = self._measure_point(cfg, psu, serial, board.ip, info.fw_version,
                                          mode_name, bool(applied.lin_enable), pct)
                self._add_row(row)
                self._emit(
                    f"{serial}: dim {pct:3d}%  F1={row.f1_w:6.3f} W  "
                    f"F2={row.f2_w:6.3f} W  total={row.total_w:6.3f} W"
                )

            # 4. teardown for this board
            try:
                client.dim(secret, serial, 0, ramp_ms=0)
                client.off(secret, serial, ramp_ms=0)
            except DriverPoEError:
                pass

    def _sweep_board_fake(self, cfg: SweepConfig, psu: Any, board: BoardRef) -> None:
        serial = board.serial or "DriverPoE-FAKE00000000"
        mode_name = DRIVER_MODE_NAMES.get(cfg.mode, str(cfg.mode))
        self._emit(f"{serial}: simulated (mode={mode_name}, lin={'on' if cfg.lin_enable else 'off'})")
        for pct in cfg.levels:
            if self._stop.is_set():
                break
            self._set_current(board=serial, dimming_pct=pct,
                              done=self.done_points, total=self.total_points)
            time.sleep(cfg.settle_s)
            row = self._measure_point(cfg, psu, serial, board.ip, "0.0.0-fake",
                                      mode_name, cfg.lin_enable, pct)
            self._add_row(row)
            self._emit(
                f"{serial}: dim {pct:3d}%  F1={row.f1_w:6.3f} W  "
                f"F2={row.f2_w:6.3f} W  total={row.total_w:6.3f} W"
            )

    def _measure_point(self, cfg: SweepConfig, psu: Any, serial: str, ip: str,
                       fw: str, mode_name: str, lin: bool, pct: int) -> Row:
        if isinstance(psu, FakePSU):
            psu._dim = pct  # let the sim track the sweep
        f1_p, f2_p = [], []
        f1_v = f1_a = f2_v = f2_a = 0.0
        n = max(1, cfg.samples)
        for k in range(n):
            m2 = psu.measure("CH2")
            m3 = psu.measure("CH3")
            f1_p.append(m2.power)
            f2_p.append(m3.power)
            f1_v, f1_a = m2.voltage, m2.current
            f2_v, f2_a = m3.voltage, m3.current
            if k < n - 1:
                time.sleep(cfg.sample_interval_s)
        f1 = statistics.fmean(f1_p)
        f2 = statistics.fmean(f2_p)
        return Row(
            board_serial=serial, board_ip=ip, fw_version=fw,
            label=cfg.label, mode_name=mode_name, lin_enable=lin,
            bus_voltage=cfg.bus_voltage, dimming_pct=pct,
            f1_w=round(f1, 4), f2_w=round(f2, 4), total_w=round(f1 + f2, 4),
            f1_v=round(f1_v, 3), f1_a=round(f1_a, 4),
            f2_v=round(f2_v, 3), f2_a=round(f2_a, 4),
            samples=n, timestamp=datetime.now().isoformat(timespec="seconds"),
        )

    def _write_csv(self) -> None:
        if not self.rows:
            return
        RESULTS_DIR.mkdir(exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        label = (self.config or {}).get("label") or "sweep"
        safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in label)[:40]
        path = RESULTS_DIR / f"{stamp}_{safe}.csv"
        with path.open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
            w.writeheader()
            w.writerows(self.rows)
        with self._lock:
            self.csv_path = str(path)
        self._emit(f"CSV written: {path.name}")


# ======================================================================= #
# Standalone entry point
# ======================================================================= #
def _cli() -> None:
    ap = argparse.ArgumentParser(description="DriverPoE dimming/power sweep")
    ap.add_argument("--psu", help="Rigol DP1308A host/IP")
    ap.add_argument("--psu-port", type=int, default=RigolDP1308A.DEFAULT_PORT)
    ap.add_argument("--board", action="append", default=[], metavar="IP",
                    help="board IP (repeatable)")
    ap.add_argument("--bus", type=float, default=52.0, help="series bus voltage (V)")
    ap.add_argument("--ilim", type=float, default=1.0, help="per-rail current limit (A)")
    ap.add_argument("--preset", choices=list(PRESETS), help="driver-config preset")
    ap.add_argument("--lin", dest="lin", action="store_true", default=True)
    ap.add_argument("--no-lin", dest="lin", action="store_false")
    ap.add_argument("--levels", help="comma-separated dim %% list")
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--samples", type=int, default=10)
    ap.add_argument("--interval", type=float, default=0.2)
    ap.add_argument("--keys", help="admin keys file")
    ap.add_argument("--fake", action="store_true", help="simulate the PSU + no real board needed")
    args = ap.parse_args()

    if not args.board and not args.fake:
        ap.error("give at least one --board (or --fake)")
    if not args.psu and not args.fake:
        ap.error("give --psu (or --fake)")

    store: Any
    if args.keys:
        store = KeyfileSecretStore()
        store.load_text(Path(args.keys).read_text(encoding="utf-8"))
    else:
        store = MemorySecretStore()

    preset = PRESETS.get(args.preset, {}) if args.preset else {}
    levels = ([int(x) for x in args.levels.split(",")] if args.levels else list(DEFAULT_LEVELS))
    cfg = SweepConfig(
        boards=[BoardRef(ip=ip) for ip in args.board] or [BoardRef(ip="fake")],
        psu_host=args.psu or "", psu_port=args.psu_port,
        bus_voltage=args.bus, current_limit_a=args.ilim,
        label=args.preset or "sweep", lin_enable=args.lin,
        levels=levels, settle_s=args.settle, samples=args.samples,
        sample_interval_s=args.interval, fake=args.fake,
        **preset,
    )

    runner = SweepRunner()
    runner.start(cfg, store)
    last = -1
    while runner.running:
        time.sleep(0.5)
        if runner.done_points != last:
            last = runner.done_points
            print(f"  {runner.done_points}/{runner.total_points}", end="\r")
    for line in runner.log:
        print(line)
    if runner.csv_path:
        print(f"\nResults: {runner.csv_path}")
    sys.exit(0 if runner.state == "done" else 1)


if __name__ == "__main__":
    _cli()
