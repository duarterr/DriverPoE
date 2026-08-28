"""A presentation-oriented control surface for DriverPoE luminaires.

Run from ``tools/`` with ``python -m webui_demo.app`` and open
http://127.0.0.1:8001.  Local-network tool.

The demo drives fixtures over **Art-Net only** and does nothing
authenticated -- no admin key at all. It discovers units with an
unauthenticated INFO broadcast, reads each one's DMX patch (universe /
start address / personality / protocols) straight out of that INFO
response, and then streams ArtDmx to them continuously (see
webui_demo/artnet.py). Commission the fixtures once in the admin UI
(tools/webui/); everything here is just writes into the Art-Net stream.
Stop the app and, after each fixture's signal-loss timeout, the admin
channel takes over again.
"""
from __future__ import annotations

import math
import threading
import time
from pathlib import Path

import numpy as np
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from device_api import discovery
from device_api.protocol import DEFAULT_PORT, DEFAULT_TIMEOUT, DMX_PROTO_ARTNET

from .artnet import ArtNetSender
from .audio_reactive import ReactiveMapper

app = FastAPI(title="DriverPoE Demo", description="Art-Net presentation controls for DriverPoE luminaires")
_static_dir = Path(__file__).resolve().parent / "static"

_sender = ArtNetSender()
_sender.start()

# The patched fixture list from the last scan, sorted by DMX address
# (universe, then start address). That order is also the band order the
# sound-reactive mode uses.
_fixtures: list[dict] = []
_fixtures_lock = threading.Lock()
_last_broadcast: str | None = None       # broadcast addr the last scan used

# Honest per-fixture readback. The Art-Net stream is fire-and-forget, so
# _sender.all_levels() is only what we're *commanding*. INFO is
# unauthenticated and reports the real state, so a slow background
# broadcast poll keeps this fresh without any authenticated call.
_reported: dict[str, dict] = {}           # ip -> {level, source, driver_on, poe_ready, layer_enabled, ts}
_reported_lock = threading.Lock()
_READBACK_TTL = 6.0                       # seconds an INFO sample stays "fresh"
_readback_misses = 0                      # consecutive readback cycles with no reply (diagnostics)

_effect_stop = threading.Event()
_effect_lock = threading.Lock()
_effect_name: str | None = None


# ======================================================================= #
# Models
# ======================================================================= #
class ScanRequest(BaseModel):
    broadcast: str | None = None


class GroupRequest(BaseModel):
    action: str                       # on | off | dim
    percent: int | None = Field(default=None, ge=0, le=100)


class IdentifyRequest(BaseModel):
    ip: str


class EffectRequest(BaseModel):
    name: str                         # wave | pulse | identify


class SpectrumItem(BaseModel):
    ip: str
    percent: int = Field(ge=0, le=100)


class SpectrumRequest(BaseModel):
    items: list[SpectrumItem] = Field(min_length=1, max_length=64)


# ======================================================================= #
# Discovery + patch -- all from the unauthenticated INFO response
# ======================================================================= #
def _probe_fixture(info) -> dict:
    """Build a fixture entry from an INFO response alone. The DMX status
    block carries universe / start address / personality / protocols, so
    no authenticated call is needed to learn the patch."""
    channels = 2 if info.dmx_personality == 1 else 1
    entry = {
        "ip": info.source_ip,
        "serial": info.serial,
        "mac": info.mac_str,
        "fw_version": info.fw_version,
        "poe_ready": info.poe_ready,
        "dmx_layer_enabled": info.dmx_layer_enabled,
        "port_address": info.dmx_artnet_port_address,
        "address": info.dmx_address,
        "personality": info.dmx_personality,
        "channels": channels,
        "controllable": False,
        "note": "",
    }
    if not info.dmx_layer_enabled:
        entry["note"] = "DMX layer disabled -- enable it in the admin UI"
    elif not (info.dmx_proto_mask & DMX_PROTO_ARTNET):
        entry["note"] = "Art-Net disabled for this fixture -- enable it in the admin UI"
    else:
        entry["controllable"] = True
        entry["note"] = f"universe 0x{info.dmx_artnet_port_address:04x}, address {info.dmx_address}"
    return entry


def _apply_patch_locked() -> None:
    _sender.set_patch([f for f in _fixtures if f["controllable"]])


def _fixture_view() -> list[dict]:
    levels = _sender.all_levels()
    now = time.time()
    with _reported_lock:
        rep = {ip: r for ip, r in _reported.items() if now - r["ts"] < _READBACK_TTL}
    with _fixtures_lock:
        out = []
        for f in _fixtures:
            r = rep.get(f["ip"])
            out.append({
                **f,
                "level": levels.get(f["ip"], 0),                        # what we're commanding
                "reported_level": r["level"] if r else None,            # what the fixture reports
                "reported_source": r["source"] if r else None,          # none|artnet|sacn|both
                "reported_fresh": r is not None,
            })
        return out


def _readback_loop() -> None:
    """Background: re-broadcast INFO (unauthenticated) every ~2 s and cache
    each fixture's real level / active source. This is the only thing that
    reflects a change made *outside* the demo (admin UI, another console,
    power loss, signal-loss fallback)."""
    while not _readback_stop.wait(2.0):
        with _fixtures_lock:
            bcast = _last_broadcast if _fixtures else None
        if not bcast:
            continue
        try:
            found = discovery.broadcast_info(bcast, DEFAULT_PORT, 1.0)
        except Exception as e:  # never let this thread die silently
            print(f"[readback] broadcast_info({bcast!r}) failed: {e!r}", flush=True)
            continue
        now = time.time()
        with _reported_lock:
            for info in found:
                _reported[info.source_ip] = {
                    "level": info.dim_percent if info.driver_on else 0,
                    "source": info.dmx_active_source,
                    "driver_on": info.driver_on,
                    "poe_ready": info.poe_ready,
                    "layer_enabled": info.dmx_layer_enabled,
                    "ts": now,
                }
        global _readback_misses
        if not found:
            _readback_misses += 1
            if _readback_misses in (1, 5, 20) or _readback_misses % 60 == 0:
                print(f"[readback] no INFO replies on {bcast} "
                      f"({_readback_misses} cycles) -- fixture bars will show the commanded level only",
                      flush=True)
        else:
            _readback_misses = 0


_readback_stop = threading.Event()


@app.on_event("startup")
def _start_readback() -> None:
    threading.Thread(target=_readback_loop, name="info-readback", daemon=True).start()


@app.post("/api/scan")
def scan(body: ScanRequest):
    global _last_broadcast
    bcast = body.broadcast or discovery.guess_broadcast_address()
    try:
        found = discovery.broadcast_info(bcast, DEFAULT_PORT, DEFAULT_TIMEOUT)
    except OSError as e:
        raise HTTPException(400, str(e))
    _last_broadcast = bcast

    entries = [_probe_fixture(info) for info in found]

    # Fixture order = the DMX patch order: universe, then start address.
    # This is also the band order the sound-reactive mode uses. Fixtures
    # that aren't on Art-Net sort last (by serial) so they don't take a
    # band slot.
    entries.sort(key=lambda e: (0, e["port_address"], e["address"]) if e["controllable"]
                 else (1, e["serial"], 0))

    with _fixtures_lock:
        _fixtures[:] = entries
        _apply_patch_locked()
    controllable = sum(1 for e in entries if e["controllable"])
    return {"devices": _fixture_view(), "controllable": controllable}


@app.get("/api/state")
def state():
    return {
        "devices": _fixture_view(),
        "sender_running": _sender.running,
        "effect": _effect_name,
    }


# ======================================================================= #
# Direct control (Art-Net stream writes -- no round trips)
# ======================================================================= #
def _controllable_ips() -> list[str]:
    with _fixtures_lock:
        return [f["ip"] for f in _fixtures if f["controllable"]]


def _resume_stream() -> None:
    """Restart the Art-Net stream if 'Blackout & release' had stopped it.
    Idempotent -- start() is a no-op while the thread is alive."""
    _sender.start()


@app.post("/api/group")
def group(body: GroupRequest):
    ips = _controllable_ips()
    if not ips:
        raise HTTPException(400, "No commissioned fixtures. Enable the DMX layer in the admin UI, then scan again.")
    _resume_stream()
    if body.action == "on":
        _sender.set_many([(ip, 100) for ip in ips])
    elif body.action == "off":
        _sender.set_many([(ip, 0) for ip in ips])
    elif body.action == "dim":
        if body.percent is None:
            raise HTTPException(400, "percent is required for dim")
        _sender.set_many([(ip, body.percent) for ip in ips])
    else:
        raise HTTPException(400, "unknown action")
    return {"ok": True, "levels": _sender.all_levels()}


@app.post("/api/spectrum")
def spectrum(body: SpectrumRequest):
    _resume_stream()
    _sender.set_many([(it.ip, it.percent) for it in body.items])
    return {"ok": True}


@app.post("/api/identify")
def identify(body: IdentifyRequest):
    if body.ip not in _controllable_ips():
        raise HTTPException(400, "fixture not commissioned for Art-Net")
    _resume_stream()
    threading.Thread(target=_identify_blink, args=(body.ip,), daemon=True).start()
    return {"ok": True}


def _identify_blink(ip: str) -> None:
    """A short high/low blink, then restore the level the fixture had."""
    restore = _sender.all_levels().get(ip, 0)
    for _ in range(3):
        _sender.set_level(ip, 100)
        time.sleep(0.18)
        _sender.set_level(ip, 0)
        time.sleep(0.18)
    _sender.set_level(ip, restore)


# ======================================================================= #
# Scenes -- loops that write the Art-Net stream over time
# ======================================================================= #
def _sleep(seconds: float) -> bool:
    """Interruptible sleep; returns True if the effect was asked to stop."""
    return _effect_stop.wait(seconds)


def _ping_pong_indices(n: int) -> list[int]:
    if n <= 1:
        return [0]
    return list(range(n)) + list(range(n - 2, 0, -1))


def _run_effect(name: str) -> None:
    global _effect_name
    try:
        ips = _controllable_ips()
        if not ips:
            return
        if name == "identify":
            order = _ping_pong_indices(len(ips))
            while not _effect_stop.is_set():
                for idx in order:
                    if _effect_stop.is_set():
                        break
                    _sender.set_many([(ip, 100 if j == idx else 0) for j, ip in enumerate(ips)])
                    if _sleep(0.35):
                        break
        elif name == "pulse":
            while not _effect_stop.is_set():
                _sender.set_many([(ip, 100) for ip in ips])
                if _sleep(0.42):
                    break
                _sender.set_many([(ip, 0) for ip in ips])
                if _sleep(0.42):
                    break
        elif name == "wave":
            # A soft raised-cosine bump whose centre sweeps smoothly back
            # and forth. Sub-stepped at ~25 fps with a wide profile and a
            # dim floor, so it breathes rather than chases.
            n = len(ips)
            span = max(1, n - 1)
            width = max(1.2, n / 3.2)          # how many fixtures the bump spans
            frames = 64                        # steps per one-way sweep (~2.6 s at 25 fps)
            pos, direction = 0.0, 1.0
            while not _effect_stop.is_set():
                out = []
                for j, ip in enumerate(ips):
                    d = min(1.0, abs(j - pos) / width)
                    v = 0.5 * (1.0 + math.cos(d * math.pi))   # 1 at centre -> 0 at width
                    out.append((ip, round(100 * v)))
                _sender.set_many(out)
                pos += direction * span / frames
                if pos >= span:
                    pos, direction = float(span), -1.0
                elif pos <= 0.0:
                    pos, direction = 0.0, 1.0
                if _sleep(0.04):
                    break
    finally:
        with _effect_lock:
            _effect_name = None


@app.post("/api/effects/start")
def start_effect(body: EffectRequest):
    global _effect_name
    if body.name not in {"identify", "pulse", "wave"}:
        raise HTTPException(400, "unknown effect")
    if not _controllable_ips():
        raise HTTPException(400, "No commissioned fixtures.")
    _resume_stream()
    with _effect_lock:
        if _effect_name:
            raise HTTPException(409, "An effect is already running. Stop it first.")
        _effect_stop.clear()
        _effect_name = body.name
        threading.Thread(target=_run_effect, args=(body.name,), daemon=True).start()
    return {"running": body.name}


@app.post("/api/effects/stop")
def stop_effect():
    _effect_stop.set()
    return {"stopping": True}


@app.get("/api/effects/status")
def effect_status():
    return {"running": _effect_name}


# ======================================================================= #
# Sound-reactive analysis (unchanged -- browser posts raw PCM)
# ======================================================================= #
@app.post("/api/music/analyze")
async def analyze_music(request: Request, sr: int, n_bands: int):
    if not 1000 <= sr <= 192000:
        raise HTTPException(400, "sr out of range")
    if not 1 <= n_bands <= 64:
        raise HTTPException(400, "n_bands out of range")
    body = await request.body()
    if len(body) < 4 or len(body) % 4 != 0:
        raise HTTPException(400, "body must be raw little-endian float32 PCM samples")
    samples = np.frombuffer(body, dtype="<f4")

    def _analyze():
        mapper = ReactiveMapper(n_bands=n_bands, sr=sr)
        _times, intensities = mapper.process_offline(samples, sr)
        return mapper, intensities

    try:
        mapper, intensities = await run_in_threadpool(_analyze)
    except Exception as e:
        raise HTTPException(400, f"analysis failed: {e}")

    percents = np.clip(np.round(intensities * 100), 0, 100).astype(int)
    return {
        "frame_seconds": mapper.hop / mapper.sr,
        "center_hz": mapper.center_hz.tolist(),
        "levels": percents.tolist(),
    }


@app.post("/api/blackout")
def blackout():
    """Stop driving entirely: kill any effect, zero the buffers, then stop
    the Art-Net stream. With no source on the wire each fixture hits its
    signal-loss timeout and the admin channel takes control back. Any
    control action (group/effect/spectrum/identify) resumes the stream."""
    _effect_stop.set()
    _sender.set_many([(ip, 0) for ip in _controllable_ips()])
    time.sleep(0.15)   # let a few explicit "0" frames go out before the stream dies
    _sender.stop()
    return {"ok": True, "sender_running": _sender.running}


@app.get("/")
def index():
    return FileResponse(_static_dir / "index.html")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("webui_demo.app:app", host="127.0.0.1", port=8001, reload=False)
