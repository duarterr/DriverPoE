"""Web UI for the DriverPoE dimming/power sweep -- FastAPI backend.

Local bench tool: binds 127.0.0.1, no login. While an admin keys file is
loaded it lives in RAM only (same contract as tools/webui/). Run from
inside tests/:

    pip install -r requirements.txt
    python server.py            # -> http://127.0.0.1:8001/

One sweep runs at a time. The browser polls /api/run/status for progress,
the live results table, and the log.

PSU note: the DP1308A is driven through its legacy HTTP Web UI protocol
(see psu_rigol.py), not SCPI -- the default PSU port is 80, and
`/api/psu` returns a live per-rail snapshot instead of a SCPI *IDN?.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent / "tools"
for _p in (str(_HERE), str(_TOOLS)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from fastapi import FastAPI, File, HTTPException, UploadFile  # noqa: E402
from fastapi.responses import FileResponse, JSONResponse  # noqa: E402
from pydantic import BaseModel, Field  # noqa: E402

from device_api import discovery  # noqa: E402
from device_api.client import AdminClient, DeviceTimeoutError, DriverPoEError  # noqa: E402
from device_api.protocol import DEFAULT_PORT, DEFAULT_TIMEOUT, DRIVER_MODE_NAMES  # noqa: E402
from device_api.secrets import KeyfileSecretStore, KeysFileError  # noqa: E402

from harness import (  # noqa: E402
    DEFAULT_LEVELS,
    PRESETS,
    BoardRef,
    SweepConfig,
    SweepRunner,
    build_matrix,
)
from psu_rigol import FakePSU, PSUError, RigolDP1308A  # noqa: E402

app = FastAPI(title="DriverPoE sweep", description="Automated dimming/power sweep bench")

_keychain = KeyfileSecretStore()
_runner = SweepRunner()
_STATIC = _HERE / "static"


# ======================================================================= #
# Keys file (RAM only)
# ======================================================================= #
@app.get("/api/keys")
def api_keys() -> dict[str, Any]:
    return {"loaded": _keychain.loaded, **_keychain.stats()}


@app.post("/api/keys")
async def api_keys_load(file: UploadFile = File(...)) -> dict[str, Any]:
    raw = (await file.read()).decode("utf-8", errors="replace")
    try:
        _keychain.load_text(raw)
    except KeysFileError as e:
        raise HTTPException(400, str(e))
    return {"loaded": True, **_keychain.stats()}


@app.post("/api/keys/clear")
def api_keys_clear() -> dict[str, Any]:
    _keychain.clear()
    return {"loaded": False, **_keychain.stats()}


# ======================================================================= #
# PSU + discovery
# ======================================================================= #
# CH2 (+25 V) and CH3 (-25 V) feed the PoE bus in series; CH1 (P6V) is idle.
_PSU_RAIL_LABEL = {"CH1": "P6V", "CH2": "P25V (Fonte 1)", "CH3": "N25V (Fonte 2)"}


@app.get("/api/psu")
def api_psu(host: str, port: int = RigolDP1308A.DEFAULT_PORT, fake: bool = False) -> dict[str, Any]:
    """Confirm the PSU answers and echo a live reading of every rail.

    With the Web UI protocol there is no *IDN?; reaching the status XML
    and parsing a plausible reading is the real reachability test.
    """
    psu = FakePSU() if fake else RigolDP1308A(host, port, timeout=4.0)
    try:
        psu.connect()
        idn = psu.idn()
        rails = {
            ch: {
                "label": _PSU_RAIL_LABEL[ch],
                "voltage": round(m.voltage, 3),
                "current": round(m.current, 3),
                "power": round(m.power, 3),
            }
            for ch in RigolDP1308A.CHANNELS
            for m in (psu.measure(ch),)
        }
    except PSUError as e:
        return JSONResponse(status_code=504, content={"error": "psu_unreachable", "message": str(e)})
    finally:
        psu.close()
    return {
        "idn": idn,
        "host": psu.host,
        "port": psu.port,
        "transport": getattr(psu, "_method", "fake"),
        "rails": rails,
    }


@app.get("/api/scan")
def api_scan(broadcast: str | None = None, timeout: float = DEFAULT_TIMEOUT) -> dict[str, Any]:
    bcast = broadcast or discovery.guess_broadcast_address()
    try:
        results = discovery.broadcast_info(bcast, DEFAULT_PORT, timeout)
    except OSError as e:
        raise HTTPException(400, f"Broadcast to {bcast} failed: {e}")
    results.sort(key=lambda info: info.serial)
    devices = [
        {
            "serial": i.serial, "ip": i.source_ip, "fw_version": i.fw_version,
            "poe_ready": i.poe_ready, "poe_source": i.poe_source,
            "dimming_mode_name": DRIVER_MODE_NAMES.get(i.dimming_mode, str(i.dimming_mode)),
            "lin_enable": i.lin_enable, "power_budget_w": i.power_budget_w,
        }
        for i in results
    ]
    return {"broadcast": bcast, "devices": devices,
            "keys": {"loaded": _keychain.loaded, **_keychain.stats()}}


@app.get("/api/presets")
def api_presets() -> dict[str, Any]:
    return {"presets": PRESETS, "default_levels": DEFAULT_LEVELS}


# ======================================================================= #
# Run control
# ======================================================================= #
class RunRequest(BaseModel):
    boards: list[str] = Field(default_factory=list)   # IPs
    psu_host: str = ""
    psu_port: int = RigolDP1308A.DEFAULT_PORT
    bus_voltage: float = 52.0
    current_limit_a: float = 1.0
    source_off_when_done: bool = True

    label: str = ""
    mode: int = 2
    pwm_freq_hz: int = 2000
    analog_freq_hz: int = 60000
    min_on_time_us: int = 20
    crossover_pct: int = 20
    power_mode: int = 0
    poe_cap_pct: int = 51
    lin_enable: bool = True

    levels: list[int] = Field(default_factory=lambda: list(DEFAULT_LEVELS))
    settle_s: float = 1.0
    samples: int = 10
    sample_interval_s: float = 0.2
    power_timeout_s: float = 20.0
    retries: int = 2
    # Run the full matrix in sequence: with/without linearization x each
    # PWM mode. When set, per-combo mode/lin_enable override the fields above.
    matrix: bool = False
    fake: bool = False


@app.post("/api/run")
def api_run(body: RunRequest) -> dict[str, Any]:
    if _runner.running:
        raise HTTPException(409, "a sweep is already running")
    if not body.fake:
        if not body.psu_host:
            raise HTTPException(400, "psu_host is required")
        if not body.boards:
            raise HTTPException(400, "select at least one board")
    if not body.levels:
        raise HTTPException(400, "levels must not be empty")
    if any(not (0 <= p <= 100) for p in body.levels):
        raise HTTPException(400, "every dim level must be 0-100")

    boards = [BoardRef(ip=ip) for ip in body.boards] or [BoardRef(ip="fake")]

    # Best-effort: resolve serials + check keys up front so the UI can show
    # them and auth fails fast. A board only answers on the network once the
    # PSU output is ON, so a timeout here is not fatal -- the runner powers
    # the bus first and resolves the serial itself.
    deferred: list[str] = []
    if not body.fake:
        for b in boards:
            try:
                with AdminClient(b.ip, DEFAULT_PORT, DEFAULT_TIMEOUT) as c:
                    info = c.info()
                    b.serial = info.serial
                    find_ok = _keychain.candidates(info.serial)
                if not find_ok:
                    raise HTTPException(401, f"{b.ip} ({info.serial}): no admin key loaded for this unit")
            except DeviceTimeoutError:
                deferred.append(b.ip)
            except DriverPoEError as e:
                raise HTTPException(400, f"{b.ip}: {e}")

    combos = build_matrix() if body.matrix else []
    run_label = body.label or ("matrix" if combos else "sweep")

    cfg = SweepConfig(
        boards=boards,
        psu_host=body.psu_host, psu_port=body.psu_port,
        bus_voltage=body.bus_voltage, current_limit_a=body.current_limit_a,
        source_off_when_done=body.source_off_when_done,
        label=run_label, mode=body.mode, pwm_freq_hz=body.pwm_freq_hz,
        analog_freq_hz=body.analog_freq_hz, min_on_time_us=body.min_on_time_us,
        crossover_pct=body.crossover_pct, power_mode=body.power_mode,
        poe_cap_pct=body.poe_cap_pct, lin_enable=body.lin_enable,
        levels=body.levels, settle_s=body.settle_s, samples=body.samples,
        sample_interval_s=body.sample_interval_s, power_timeout_s=body.power_timeout_s,
        retries=body.retries, combos=combos,
        fake=body.fake,
    )
    started = _runner.start(cfg, _keychain)
    if not started:
        raise HTTPException(409, "a sweep is already running")
    return {
        "started": True,
        "boards": [b.serial or b.ip for b in boards],
        "deferred": deferred,
        "combos": [c.label for c in combos],
    }


@app.get("/api/run/status")
def api_run_status() -> dict[str, Any]:
    return _runner.snapshot()


@app.post("/api/run/stop")
def api_run_stop() -> dict[str, Any]:
    _runner.stop()
    return {"stopping": True}


@app.get("/api/run/report")
def api_run_report():
    snap = _runner.snapshot()
    path = snap.get("report_path")
    if not path or not Path(path).exists():
        raise HTTPException(404, "no report available yet")
    return FileResponse(
        path,
        media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        filename=Path(path).name,
    )


@app.get("/")
def index():
    return FileResponse(_STATIC / "index.html")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("server:app", host="127.0.0.1", port=8001, reload=False)
