"""A small, presentation-oriented control surface for DriverPoE luminaires.

Run from ``tools/`` with ``python -m webui_demo.app`` and open
http://127.0.0.1:8001.  This is intentionally a local-network tool.
"""
from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, Field
from starlette.concurrency import run_in_threadpool

from device_api import discovery
from device_api.client import (
    AdminClient, AuthError, CommandRefusedError, DeviceTimeoutError,
    DriverPoEError, find_working_secret,
)
from device_api.protocol import DEFAULT_PORT, DEFAULT_RAMP_MS, DEFAULT_TIMEOUT, SECRET_LEN, ProtocolVersionMismatchError
from device_api.secrets import DEFAULT_SECRETS_FILE, JsonFileSecretStore

from .audio_reactive import ReactiveMapper

app = FastAPI(title="DriverPoE Demo", description="Presentation controls for DriverPoE luminaires")
_static_dir = Path(__file__).resolve().parent / "static"
_secret_store = JsonFileSecretStore(DEFAULT_SECRETS_FILE)
_effect_stop = threading.Event()
_effect_lock = threading.Lock()
_effect_name: str | None = None

# (serial, secret) per IP, filled in by the first successful _command() call.
# _command() uses this to skip INFO + secret-verification on every command
# after the first one to a given ip -- see _fast_command(). Without it, each
# command costs up to four UDP round trips (INFO, a verifying CHALLENGE, the
# write command's own CHALLENGE, then the write itself), which at anything
# above a few commands/second trips the firmware's per-source rate limiter
# (admin_channel.c: 20 packets/second/IP) and every command stalls for the
# full 3s socket timeout waiting on a response that was silently dropped --
# the lights look "stuck", and effects can't react to /api/effects/stop until
# whatever's in flight finishes.
_ip_cache: dict[str, tuple[str, bytes]] = {}
_ip_cache_lock = threading.Lock()
FAST_TIMEOUT = 0.35


class AuthNeeded(Exception):
    pass


class GroupRequest(BaseModel):
    ips: list[str] = Field(min_length=1, max_length=64)
    action: str
    percent: int | None = Field(default=None, ge=0, le=100)
    ramp_ms: int = Field(default=DEFAULT_RAMP_MS, ge=0, le=10000)
    secret_hex: str | None = None


class EffectRequest(BaseModel):
    ips: list[str] = Field(min_length=1, max_length=64)
    name: str
    secret_hex: str | None = None


class SpectrumItem(BaseModel):
    ip: str
    percent: int = Field(ge=0, le=100)


class SpectrumRequest(BaseModel):
    items: list[SpectrumItem] = Field(min_length=1, max_length=64)
    # 0 = instant duty change (hv9910.c's hv_do_set_dim skips the hardware
    # fade entirely when ramp_ms==0) -- the music-follow effect wants each
    # update to land immediately, not lag behind the beat by its own ramp.
    ramp_ms: int = Field(default=0, ge=0, le=10000)
    secret_hex: str | None = None


def _device_dict(info):
    return {"serial": info.serial, "ip": info.source_ip, "mac": info.mac_str,
            "fw_version": info.fw_version, "on": info.driver_on,
            "dim_percent": info.dim_percent, "poe_ready": info.poe_ready}


def _resolve_secret(client: AdminClient, serial: str, secret_hex: str | None) -> bytes:
    used_manual = False

    def manual() -> bytes:
        nonlocal used_manual
        used_manual = True
        if not secret_hex:
            raise AuthNeeded()
        try:
            value = bytes.fromhex(secret_hex)
        except ValueError:
            raise HTTPException(400, "secret_hex must be hexadecimal")
        if len(value) != SECRET_LEN:
            raise HTTPException(400, f"secret_hex must contain {SECRET_LEN} bytes")
        return value

    secret, _ = find_working_secret(client, serial, _secret_store, manual_secret_provider=manual)
    if used_manual:
        _secret_store.set(serial, secret)
    return secret


def _error(e: Exception) -> JSONResponse:
    if isinstance(e, AuthNeeded):
        return JSONResponse(status_code=401, content={"error": "auth_needed", "message": "Informe o segredo admin para as luminárias ainda não conhecidas."})
    if isinstance(e, DeviceTimeoutError):
        return JSONResponse(status_code=504, content={"error": "timeout", "message": str(e)})
    if isinstance(e, ProtocolVersionMismatchError):
        return JSONResponse(status_code=409, content={"error": "version_mismatch", "message": str(e)})
    if isinstance(e, (AuthError, CommandRefusedError, DriverPoEError)):
        return JSONResponse(status_code=400, content={"error": "device_error", "message": str(e)})
    raise e


def _fast_command(ip: str, serial: str, secret: bytes, action: str, percent: int | None,
                   ramp_ms: int, timeout: float, persist: bool) -> dict:
    # No INFO, no secret re-verification -- just the CHALLENGE + one write command
    # the protocol requires for a fresh anti-replay nonce (2 packets in, 2 out,
    # instead of the 4 round trips _command() needs the first time it sees an ip).
    with AdminClient(ip, DEFAULT_PORT, timeout) as client:
        if action == "on":
            result = client.on(secret, serial, ramp_ms)
        elif action == "off":
            result = client.off(secret, serial, ramp_ms)
        elif action == "dim":
            if percent is None:
                raise HTTPException(400, "percent is required for dim")
            result = client.dim(secret, serial, percent, ramp_ms, persist=persist)
        elif action == "identify":
            result = client.identify(secret, serial)
        else:
            raise HTTPException(400, "unknown action")
        return {"ip": ip, "serial": serial, "ok": result.accepted, "status": result.status.name}


def _command(ip: str, action: str, percent: int | None, ramp_ms: int, secret_hex: str | None,
             persist: bool = True) -> dict:
    # Once an ip's serial/secret are cached, every command -- from effects,
    # group actions, and /api/spectrum alike -- takes the fast, short-timeout
    # path. This matters beyond raw speed: effects' "Stop" used to be able to
    # wait up to DEFAULT_TIMEOUT (3s) for one in-flight slow command to finish
    # before it could even check whether it should stop; bounding that to
    # FAST_TIMEOUT makes Stop feel immediate. It also halves the packets per
    # command, which helps timing stay consistent under the firmware's
    # per-source rate limit (admin_channel.c).
    #
    # persist=False (effects, /api/spectrum -- anything that dims at more than
    # a couple of Hz) marks the dim as transient, so the device skips writing
    # it to NVS. That write is a blocking flash commit on the same task that
    # drains the device's own command queue -- a rapid stream of persisted
    # dims can make that queue back up, so the light lags further and further
    # behind what was actually requested instead of tracking it in real time,
    # and "stop" has to wait for the backlog to drain instead of taking effect
    # on the next command.
    with _ip_cache_lock:
        cached = _ip_cache.get(ip)
    if cached is not None:
        serial, secret = cached
        try:
            return _fast_command(ip, serial, secret, action, percent, ramp_ms, FAST_TIMEOUT, persist)
        except HTTPException:
            raise
        except AuthError:
            # The cached secret stopped working (e.g. rotated elsewhere) --
            # drop it and fall through to a full resolve below.
            with _ip_cache_lock:
                _ip_cache.pop(ip, None)
        except Exception as e:
            error = "timeout" if isinstance(e, DeviceTimeoutError) else str(e)
            return {"ip": ip, "ok": False, "error": error}

    # First time we've seen this ip (or its cached secret just failed): the
    # slow, full path -- INFO + secret resolution -- populates _ip_cache for
    # every command after this one.
    try:
        with AdminClient(ip, DEFAULT_PORT, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, secret_hex)
            with _ip_cache_lock:
                _ip_cache[ip] = (info.serial, secret)
            return _fast_command(ip, info.serial, secret, action, percent, ramp_ms, DEFAULT_TIMEOUT, persist)
    except HTTPException:
        raise
    except Exception as e:
        return {"ip": ip, "ok": False, "error": "auth_needed" if isinstance(e, AuthNeeded) else str(e)}


def _group(ips: list[str], action: str, percent: int | None, ramp_ms: int, secret_hex: str | None,
           persist: bool = True) -> list[dict]:
    # One socket/client per fixture makes these commands reach the fixtures together,
    # rather than serialising a room-sized lighting cue over UDP.
    with ThreadPoolExecutor(max_workers=min(16, len(ips))) as pool:
        futures = [pool.submit(_command, ip, action, percent, ramp_ms, secret_hex, persist) for ip in ips]
        return [future.result() for future in as_completed(futures)]


def _spectrum(items: list[SpectrumItem], ramp_ms: int, secret_hex: str | None) -> list[dict]:
    # Like _group, but each fixture gets its own percent — used so every luminaire
    # can track a different slice of the audio spectrum instead of one shared level.
    # _command() already prefers the cached fast path for every repeat ip.
    # persist=False always: this is the music-reactive path, dimming at 5Hz --
    # see _command()'s comment for why that must never hit NVS per update.
    with ThreadPoolExecutor(max_workers=min(16, len(items))) as pool:
        futures = [pool.submit(_command, it.ip, "dim", it.percent, ramp_ms, secret_hex, False) for it in items]
        return [future.result() for future in as_completed(futures)]


@app.get("/api/scan")
def scan(broadcast: str | None = None):
    try:
        found = discovery.broadcast_info(broadcast or discovery.guess_broadcast_address(), DEFAULT_PORT, DEFAULT_TIMEOUT)
    except OSError as e:
        raise HTTPException(400, str(e))
    return {"devices": [_device_dict(d) for d in sorted(found, key=lambda d: d.serial)]}


@app.post("/api/group")
def group(body: GroupRequest):
    return {"results": _group(body.ips, body.action, body.percent, body.ramp_ms, body.secret_hex)}


@app.post("/api/spectrum")
def spectrum(body: SpectrumRequest):
    return {"results": _spectrum(body.items, body.ramp_ms, body.secret_hex)}


@app.post("/api/music/analyze")
async def analyze_music(request: Request, sr: int, n_bands: int):
    # The browser decodes the picked file and posts the raw PCM itself (see
    # index.html) -- this endpoint never touches an audio file directly, just
    # numbers, so it doesn't need librosa/ffmpeg or any format-specific code.
    if not 1000 <= sr <= 192000:
        raise HTTPException(400, "sr out of range")
    if not 1 <= n_bands <= 64:
        raise HTTPException(400, "n_bands out of range")
    body = await request.body()
    if len(body) < 4 or len(body) % 4 != 0:
        raise HTTPException(400, "body must be raw little-endian float32 PCM samples")
    samples = np.frombuffer(body, dtype="<f4")

    def _analyze():
        # ReactiveMapper.process_offline() runs a Python-level loop with an
        # FFT per frame -- real CPU work, not I/O -- so it goes through the
        # threadpool instead of blocking the event loop (and every other
        # request, e.g. Stop, that would otherwise have to wait behind it).
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


def _ping_pong_indices(n: int) -> list[int]:
    """0..n-1 then back down to 1 (not repeating either endpoint) -- looping
    this list forever gives a smooth 1->N->1->N->... sweep with no double
    dwell at either end. n<=1 has nowhere to bounce, so it's just [0]."""
    if n <= 1:
        return [0]
    return list(range(n)) + list(range(n - 2, 0, -1))


def _run_effect(body: EffectRequest) -> None:
    # Every branch loops until /api/effects/stop sets _effect_stop — cues are meant
    # to run indefinitely during a demo rather than finish after a fixed count.
    global _effect_name
    try:
        if body.name == "identify":
            # A 1..N..1 chase: exactly one fixture lit at a time, enforced by
            # ORDER, not just by timing -- turn every other fixture off and
            # wait for that to land, THEN turn the target on, instead of
            # broadcasting the whole next state in parallel (which can't
            # guarantee the old fixture's OFF lands before the new one's ON).
            # The OFF step stays ramp_ms=0 (instant cut) so there's no window
            # where the outgoing fixture is still visibly fading -- that's
            # the part that must never overlap. The ON step gets a short fade
            # instead of also snapping instantly, just for some visual life.
            order = _ping_pong_indices(len(body.ips))
            while not _effect_stop.is_set():
                for idx in order:
                    if _effect_stop.is_set():
                        break
                    target_ip = body.ips[idx]
                    other_ips = [ip for j, ip in enumerate(body.ips) if j != idx]
                    if other_ips:
                        _group(other_ips, "dim", 0, 0, body.secret_hex, persist=False)
                    if _effect_stop.is_set():
                        break
                    _command(target_ip, "dim", 100, 90, body.secret_hex, persist=False)
                    if _effect_stop.wait(0.35):
                        break
        elif body.name == "pulse":
            # dim(0) with a nonzero ramp genuinely reaches SHUTDOWN once the
            # hardware fade finishes (hv9910.c's hv_fade_end_cb), regardless of
            # when the next command arrives -- see that file for the fix.
            while not _effect_stop.is_set():
                _group(body.ips, "dim", 100, 180, body.secret_hex, persist=False)
                if _effect_stop.wait(0.48): break
                _group(body.ips, "dim", 0, 260, body.secret_hex, persist=False)
                if _effect_stop.wait(0.48): break
        elif body.name == "wave":
            # A brightness peak sweeps 1->N->1->... forever; each step sends
            # every fixture's full target percent at once (see the identify
            # branch above for why full-state-per-step, not incremental,
            # avoids drift from a dropped packet). A fixture's percent falls
            # off with its distance from the current peak, so 2-3 neighbours
            # are partially lit as the peak passes -- an actual travelling
            # wave shape, 0-100%, instead of one isolated spotlight (that's
            # what "identify" above already does). Unlike identify, there's
            # no "never overlap" constraint here -- overlap between
            # neighbours IS the wave -- so a fade is safe, and it's what
            # makes consecutive steps read as continuous motion instead of a
            # slideshow of static frames.
            FALLOFF_PER_STEP = 30
            order = _ping_pong_indices(len(body.ips))
            while not _effect_stop.is_set():
                for peak in order:
                    if _effect_stop.is_set():
                        break
                    items = [SpectrumItem(ip=ip, percent=max(0, 100 - abs(j - peak) * FALLOFF_PER_STEP))
                              for j, ip in enumerate(body.ips)]
                    _spectrum(items, 180, body.secret_hex)
                    if _effect_stop.wait(0.22):
                        break
    finally:
        with _effect_lock:
            _effect_name = None


@app.post("/api/effects/start")
def start_effect(body: EffectRequest):
    global _effect_name
    if body.name not in {"identify", "pulse", "wave"}:
        raise HTTPException(400, "unknown effect")
    with _effect_lock:
        if _effect_name:
            raise HTTPException(409, "An effect is already running. Stop it first.")
        _effect_stop.clear()
        _effect_name = body.name
        threading.Thread(target=_run_effect, args=(body,), daemon=True).start()
    return {"running": body.name}


@app.post("/api/effects/stop")
def stop_effect():
    _effect_stop.set()
    return {"stopping": True}


@app.get("/api/effects/status")
def effect_status():
    return {"running": _effect_name}


@app.get("/")
def index():
    return FileResponse(_static_dir / "index.html")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("webui_demo.app:app", host="127.0.0.1", port=8001, reload=False)
