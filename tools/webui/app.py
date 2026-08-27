"""DriverPoE local web UI -- FastAPI backend.

Built entirely on top of the `device_api` package (../device_api/) -- no
protocol/HMAC logic of its own. Nothing is stored: the operator uploads a
plain-text keys file (POST /api/keys) each time they open the page, and
it lives in RAM for the life of this process only. Format, one per line:

    DriverPoE-A4CF12B93D08   b64c3218...        # by full serial
    A4CF12B93D08             0011223344...      # or by MAC tail
    all others               ffffffffffff...    # fallback for the rest

The backend tries the matching key per unit and reports each card's auth
status (key / factory-default / no key). A key is only ever returned to
the browser once, right after a CHANGE_SECRET the operator requested.

Run from inside tools/:

    pip install fastapi "uvicorn[standard]" cryptography
    python -m webui.app

...or `uvicorn webui.app:app --reload` from inside tools/. Then open
http://127.0.0.1:8000/ .

This is a LOCAL tool: it binds 127.0.0.1 and has no operator login --
while a keys file is loaded, anything that can reach this HTTP server can
command the units those keys unlock. Run it on the operator's own
machine; don't expose the port.
"""
from __future__ import annotations

import socket
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel

try:
    import psutil
except ImportError:  # only needed for /api/interfaces (the broadcast-address dropdown)
    psutil = None

from device_api import discovery
from device_api.client import (
    AdminClient,
    AuthError,
    CommandRefusedError,
    DeviceTimeoutError,
    DriverPoEError,
    MissingDependencyError,
    OtaTransferError,
    find_working_secret,
)
from device_api.models import CommandResult, DeviceInfo
from device_api.protocol import (
    DEFAULT_PORT,
    DEFAULT_RAMP_MS,
    DEFAULT_TIMEOUT,
    SECRET_LEN,
    DmxConfig,
    ProtocolVersionMismatchError,
)
from device_api.secrets import ADMIN_DEFAULT_SECRET, KeyfileSecretStore, KeysFileError

app = FastAPI(title="DriverPoE", description="Local admin UI for DriverPoE luminaires")

# Per-unit admin keys come from a plain-text keys file the operator
# uploads (POST /api/keys) each time they open the page. It's held in RAM
# only -- never written anywhere. A "*" / "any" / "all others" line in
# that file is the fallback key tried for serials not listed explicitly.
_keychain = KeyfileSecretStore()

_STATIC_DIR = Path(__file__).resolve().parent / "static"

# In-memory OTA progress, keyed by device IP -- lets the browser poll
# /ota/progress while a (synchronous, potentially several-second) upload
# is in flight on a worker thread, without needing WebSockets/SSE. Never
# persisted, never holds anything secret. Plain dict assignment is enough
# thread-safety here (CPython's GIL makes it atomic) -- this is a casual
# local status readout, not a concurrency-critical path.
_ota_progress: dict[str, dict[str, Any]] = {}


class AuthNeeded(Exception):
    """Neither a stored nor the factory-default secret worked, and the
    request didn't supply one to try -- the frontend should prompt the
    operator for one and resubmit with secret_hex set."""


def _device_to_dict(info: DeviceInfo) -> dict[str, Any]:
    """JSON-safe view of a DeviceInfo -- never includes anything
    secret-related (there's nothing secret in DeviceInfo to begin with)."""
    return {
        "serial": info.serial,
        "source_ip": info.source_ip,
        "mac": info.mac_str,
        "fw_version": info.fw_version,
        "ip": info.ip,
        "uptime_s": info.uptime_s,
        "reset_reason": info.reset_reason,
        "poe_ready": info.poe_ready,
        "poe_source": info.poe_source,
        "poe_cdb_confirmed": info.poe_cdb_confirmed,
        "poe_t2p_confirmed": info.poe_t2p_confirmed,
        "poe_vbus_confirmed": info.poe_vbus_confirmed,
        "power_blocking_reason": info.power_blocking_reason,
        "driver_on": info.driver_on,
        "desired_on": info.desired_on,
        "dim_percent": info.dim_percent,
        "ramp_pending": info.ramp_pending,
        "vbus_mv": info.vbus_mv,
        "led_voltage_mv": info.led_voltage_mv,
        "dmx_layer_enabled": info.dmx_layer_enabled,
        "dmx_active_source": info.dmx_active_source,
        "dmx_level": info.dmx_level,
        "dmx_fps": info.dmx_fps,
        "dmx_artnet_port_address": info.dmx_artnet_port_address,
        "dmx_sacn_universe": info.dmx_sacn_universe,
        "dmx_last_src_ip": info.dmx_last_src_ip,
        "dmx_address": info.dmx_address,
        "dmx_personality": info.dmx_personality,
        "dmx_proto_mask": info.dmx_proto_mask,
    }


def _dmx_to_dict(cfg: DmxConfig) -> dict[str, Any]:
    return {
        "layer_enabled": cfg.layer_enabled,
        "proto_mask": cfg.proto_mask,
        "artnet_port_address": cfg.artnet_port_address,
        "artnet_net": cfg.artnet_net,
        "artnet_subnet": cfg.artnet_subnet,
        "artnet_universe": cfg.artnet_universe,
        "sacn_universe": cfg.sacn_universe,
        "dmx_address": cfg.dmx_address,
        "personality": cfg.personality,
        "merge_mode": cfg.merge_mode,
        "loss_behavior": cfg.loss_behavior,
        "loss_level": cfg.loss_level,
        "loss_timeout_ms": cfg.loss_timeout_ms,
        "smoothing_ms": cfg.smoothing_ms,
        "allow_artaddress": cfg.allow_artaddress,
    }


def _result_to_dict(result: CommandResult) -> dict[str, Any]:
    return {
        "command": result.command,
        "serial": result.serial,
        "status": result.status.name,
        "accepted": result.accepted,
        "applied": result.applied,
        "pending": result.pending,
    }


def _resolve_secret(client: AdminClient, serial: str, secret_hex: str | None) -> bytes:
    """Tries the stored/factory-default secret first; if both fail and
    secret_hex was supplied, tries that (and remembers it on success, so
    the next request against this unit doesn't need it again). Raises
    AuthNeeded if nothing worked and no secret_hex was given -- the
    frontend's cue to ask the operator for one."""
    manual_was_used = False

    def manual_provider() -> bytes:
        nonlocal manual_was_used
        manual_was_used = True
        if not secret_hex:
            raise AuthNeeded()
        try:
            secret = bytes.fromhex(secret_hex)
        except ValueError:
            raise HTTPException(400, "secret_hex must be a 64-character hex string")
        if len(secret) != SECRET_LEN:
            raise HTTPException(400, f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
        return secret

    try:
        secret, _nonce = find_working_secret(client, serial, _keychain, manual_secret_provider=manual_provider)
    except AuthError:
        if manual_was_used and secret_hex:
            raise HTTPException(401, "The provided admin secret was rejected by the device")
        raise
    if manual_was_used and secret_hex:
        _keychain.set(serial, secret)   # session only -- nothing written
    return secret


def _error_response(e: Exception) -> JSONResponse:
    if isinstance(e, AuthNeeded):
        return JSONResponse(status_code=401, content={"error": "auth_needed",
                             "message": "No working admin secret for this unit -- supply one (secret_hex) to try."})
    if isinstance(e, DeviceTimeoutError):
        return JSONResponse(status_code=504, content={"error": "timeout", "message": str(e)})
    if isinstance(e, ProtocolVersionMismatchError):
        return JSONResponse(status_code=409, content={
            "error": "version_mismatch",
            "message": str(e),
            "device_version": e.got,
            "package_version": e.expected,
        })
    if isinstance(e, CommandRefusedError):
        return JSONResponse(status_code=409, content={"error": "refused", "status": e.status.name, "message": str(e)})
    if isinstance(e, MissingDependencyError):
        return JSONResponse(status_code=501, content={"error": "missing_dependency", "message": str(e)})
    if isinstance(e, OtaTransferError):
        return JSONResponse(status_code=409, content={"error": "ota_transfer_failed", "message": str(e)})
    if isinstance(e, DriverPoEError):
        return JSONResponse(status_code=400, content={"error": "device_api_error", "message": str(e)})
    raise e


class OnOffRequest(BaseModel):
    ramp_ms: int = DEFAULT_RAMP_MS
    secret_hex: str | None = None


class DimRequest(BaseModel):
    percent: int
    ramp_ms: int = DEFAULT_RAMP_MS
    secret_hex: str | None = None


class SecretOnlyRequest(BaseModel):
    secret_hex: str | None = None


class FactoryResetRequest(BaseModel):
    confirm_serial: str
    secret_hex: str | None = None


class ChangeSecretRequest(BaseModel):
    new_secret_hex: str | None = None  # None -> device generates/we generate a random one
    secret_hex: str | None = None      # current secret, if neither stored nor factory default works


class DmxConfigRequest(BaseModel):
    layer_enabled: bool = False
    proto_mask: int = 0x03
    # Art-Net universe expressed as parts (net/subnet/universe); the backend
    # packs them into the 15-bit port address.
    artnet_net: int = 0
    artnet_subnet: int = 0
    artnet_universe: int = 0
    sacn_universe: int = 1
    dmx_address: int = 1
    personality: int = 0
    merge_mode: int = 0
    loss_behavior: int = 0
    loss_level: int = 0
    loss_timeout_ms: int = 3000
    smoothing_ms: int = 25
    allow_artaddress: bool = True
    secret_hex: str | None = None


# ======================================================================= #
# Keys file (RAM only, never written)
# ======================================================================= #
@app.get("/api/keys")
def api_keys():
    return {"loaded": _keychain.loaded, **_keychain.stats()}


@app.post("/api/keys")
async def api_keys_load(file: UploadFile = File(...)):
    """Load a keys file (see the module docstring for the format). Replaces
    whatever was loaded before. Nothing is written to disk."""
    raw = (await file.read()).decode("utf-8", errors="replace")
    try:
        _keychain.load_text(raw)
    except KeysFileError as e:
        raise HTTPException(400, str(e))
    return {"loaded": True, **_keychain.stats()}


@app.post("/api/keys/clear")
def api_keys_clear():
    _keychain.clear()
    return {"loaded": False, **_keychain.stats()}


def _probe_auth(ip: str, serial: str) -> str:
    """'key' = a real per-unit/fallback key from the loaded file works;
    'default' = only the compiled-in factory default works (unit is not
    protected); 'none' = nothing we have unlocks it."""
    try:
        with AdminClient(ip, DEFAULT_PORT, 1.5) as client:
            secret, _ = find_working_secret(client, serial, _keychain)
        return "default" if secret == ADMIN_DEFAULT_SECRET else "key"
    except (AuthError, DeviceTimeoutError, DriverPoEError):
        return "none"


# ======================================================================= #
# Discovery
# ======================================================================= #
def _broadcast_for(address: str, netmask: str) -> str | None:
    """IPv4 broadcast address for a given (address, netmask) pair --
    address | ~netmask, octet by octet. None if either isn't a well-formed
    dotted-quad (psutil can report non-IPv4-looking values for some
    virtual adapters)."""
    try:
        ip = [int(o) for o in address.split(".")]
        mask = [int(o) for o in netmask.split(".")]
    except ValueError:
        return None
    if len(ip) != 4 or len(mask) != 4:
        return None
    return ".".join(str(ip[i] | (~mask[i] & 0xFF)) for i in range(4))


@app.get("/api/interfaces")
def api_interfaces():
    """Every local IPv4 interface's broadcast address -- powers the
    broadcast-address dropdown in the UI. Needed because guessing a single
    "the" broadcast address (discovery.guess_broadcast_address(), based on
    whichever interface the OS would pick for internet-bound traffic) is
    wrong on a multi-homed machine: e.g. a wired connection to one router
    while still on Wi-Fi to the router the luminaire is actually on -- the
    OS usually prefers wired for its default route, so the auto-guess
    silently points at the wrong network. Loopback (127.0.0.0/8) and
    link-local/APIPA (169.254.0.0/16 -- an interface with no real DHCP
    lease) are excluded; they're never useful here."""
    if psutil is None:
        return {"interfaces": [], "error": "psutil not installed -- pip install psutil"}
    seen = set()
    results = []
    for name, addrs in psutil.net_if_addrs().items():
        for addr in addrs:
            if addr.family != socket.AF_INET:
                continue
            ip = addr.address
            if not ip or ip.startswith("127.") or ip.startswith("169.254."):
                continue
            broadcast = addr.broadcast or (_broadcast_for(ip, addr.netmask) if addr.netmask else None)
            if not broadcast or broadcast in seen:
                continue
            seen.add(broadcast)
            results.append({"interface": name, "address": ip, "netmask": addr.netmask, "broadcast": broadcast})
    return {"interfaces": results}


@app.get("/api/scan")
def api_scan(broadcast: str | None = None, timeout: float = DEFAULT_TIMEOUT):
    bcast = broadcast or discovery.guess_broadcast_address()
    try:
        results = discovery.broadcast_info(bcast, DEFAULT_PORT, timeout)
    except OSError as e:
        raise HTTPException(400, f"Broadcast to {bcast} failed: {e}")
    results.sort(key=lambda info: info.serial)

    # Probe each unit's auth status in parallel (CHALLENGE only, no change).
    auth: dict[str, str] = {}
    if results:
        with ThreadPoolExecutor(max_workers=min(16, len(results))) as pool:
            for info, st in zip(results, pool.map(lambda i: _probe_auth(i.source_ip, i.serial), results)):
                auth[info.serial] = st

    devices = [{**_device_to_dict(info), "auth": auth.get(info.serial, "unknown")} for info in results]
    return {"broadcast": bcast, "devices": devices, "keys": {"loaded": _keychain.loaded, **_keychain.stats()}}


@app.get("/api/devices/{ip}/info")
def api_device_info(ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT):
    try:
        with AdminClient(ip, port, timeout) as client:
            info = client.info()
    except Exception as e:
        return _error_response(e)
    return {**_device_to_dict(info), "auth": _probe_auth(ip, info.serial)}


# ======================================================================= #
# Commands
# ======================================================================= #
@app.post("/api/devices/{ip}/on")
def api_on(ip: str, body: OnOffRequest, port: int = DEFAULT_PORT):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.on(secret, info.serial, body.ramp_ms)
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/off")
def api_off(ip: str, body: OnOffRequest, port: int = DEFAULT_PORT):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.off(secret, info.serial, body.ramp_ms)
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/dim")
def api_dim(ip: str, body: DimRequest, port: int = DEFAULT_PORT):
    if not 0 <= body.percent <= 100:
        raise HTTPException(400, "percent must be 0-100")
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.dim(secret, info.serial, body.percent, body.ramp_ms)
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/identify")
def api_identify(ip: str, body: SecretOnlyRequest, port: int = DEFAULT_PORT):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.identify(secret, info.serial)
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/reboot")
def api_reboot(ip: str, body: SecretOnlyRequest, port: int = DEFAULT_PORT):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.reboot(secret, info.serial)
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/factory_reset")
def api_factory_reset(ip: str, body: FactoryResetRequest, port: int = DEFAULT_PORT):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            # Explicit confirmation, mirroring the CLI's "type the exact
            # serial" prompt -- a destructive action must never fire from
            # a single accidental click.
            if body.confirm_serial != info.serial:
                raise HTTPException(400, f"confirm_serial must exactly match the device's serial ({info.serial})")
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.factory_reset(secret, info.serial)
            if result.applied:
                _keychain.delete(info.serial)  # device reverted its secret to the factory default too
    except Exception as e:
        return _error_response(e)
    return _result_to_dict(result)


@app.post("/api/devices/{ip}/change_secret")
def api_change_secret(ip: str, body: ChangeSecretRequest, port: int = DEFAULT_PORT):
    new_secret = None
    if body.new_secret_hex:
        try:
            new_secret = bytes.fromhex(body.new_secret_hex)
        except ValueError:
            raise HTTPException(400, "new_secret_hex must be a 64-character hex string")
        if len(new_secret) != SECRET_LEN:
            raise HTTPException(400, f"new secret must be {SECRET_LEN} bytes; got {len(new_secret)}")
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result, applied_secret = client.change_secret(secret, info.serial, new_secret)
            if result.applied:
                _keychain.set(info.serial, applied_secret)  # session only
    except Exception as e:
        return _error_response(e)
    # The new secret is returned ONCE, directly to the operator who just
    # requested the change -- this is the one deliberate exception to
    # "never expose the admin secret to the frontend": the browser that
    # asked for a new secret needs to see it to hand it to whoever
    # installs/labels the unit. It is never returned by any other
    # endpoint, never logged, and this response is never cached (no
    # GET involved).
    return {**_result_to_dict(result), "new_secret_hex": applied_secret.hex() if result.applied else None}


# ======================================================================= #
# DMX / Art-Net / sACN layer
# ======================================================================= #
@app.get("/api/devices/{ip}/dmx")
def api_dmx_get(ip: str, port: int = DEFAULT_PORT, secret_hex: str | None = None):
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, secret_hex)
            cfg = client.get_dmx_config(secret, info.serial)
    except Exception as e:
        return _error_response(e)
    return _dmx_to_dict(cfg)


@app.post("/api/devices/{ip}/dmx")
def api_dmx_set(ip: str, body: DmxConfigRequest, port: int = DEFAULT_PORT):
    cfg = DmxConfig(
        layer_enabled=body.layer_enabled,
        proto_mask=body.proto_mask,
        artnet_port_address=DmxConfig.from_artnet_parts(
            body.artnet_net, body.artnet_subnet, body.artnet_universe),
        sacn_universe=body.sacn_universe,
        dmx_address=body.dmx_address,
        personality=body.personality,
        merge_mode=body.merge_mode,
        loss_behavior=body.loss_behavior,
        loss_level=body.loss_level,
        loss_timeout_ms=body.loss_timeout_ms,
        smoothing_ms=body.smoothing_ms,
        allow_artaddress=body.allow_artaddress,
    )
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, body.secret_hex)
            result = client.set_dmx_config(secret, info.serial, cfg)
            applied = client.get_dmx_config(secret, info.serial) if result.applied else cfg
    except Exception as e:
        return _error_response(e)
    return {**_result_to_dict(result), "config": _dmx_to_dict(applied)}


# ======================================================================= #
# OTA
# ======================================================================= #
@app.post("/api/devices/{ip}/ota/upload")
def api_ota_upload(ip: str, file: bytes = File(...), secret_hex: str | None = Form(None), port: int = DEFAULT_PORT):
    """Pushes `file`'s bytes to the device as a new firmware image and
    stages it (does NOT reboot -- see device_api.client.AdminClient.
    ota_update()'s docstring). A plain `def` (not `async def`) on purpose:
    FastAPI runs synchronous routes in a worker thread automatically, so
    this potentially multi-second blocking UDP transfer never stalls the
    event loop -- a concurrent GET to /ota/progress below keeps working
    the whole time. `file: bytes = File(...)` (not UploadFile) for the
    same reason -- UploadFile's async read API doesn't fit a sync route,
    and firmware images here are small enough (under a couple MB) to hold
    in memory whole without concern."""
    if not file:
        raise HTTPException(400, "uploaded file is empty")

    _ota_progress[ip] = {"state": "starting", "bytes_sent": 0, "total_bytes": len(file), "message": ""}
    try:
        with AdminClient(ip, port, DEFAULT_TIMEOUT) as client:
            info = client.info()
            secret = _resolve_secret(client, info.serial, secret_hex)
            _ota_progress[ip]["state"] = "uploading"

            def progress_cb(sent: int, total: int) -> None:
                _ota_progress[ip].update(bytes_sent=sent, total_bytes=total)

            result = client.ota_update(secret, info.serial, file, progress_callback=progress_cb)
    except Exception as e:
        _ota_progress[ip] = {"state": "error", "bytes_sent": _ota_progress.get(ip, {}).get("bytes_sent", 0),
                              "total_bytes": len(file), "message": str(e)}
        return _error_response(e)

    if result.applied:
        _ota_progress[ip] = {"state": "done", "bytes_sent": len(file), "total_bytes": len(file),
                              "message": "Image validated and staged -- reboot to apply."}
    else:
        _ota_progress[ip] = {"state": "error", "bytes_sent": len(file), "total_bytes": len(file),
                              "message": f"Device refused the image: {result.status.name}"}
    return _result_to_dict(result)


@app.get("/api/devices/{ip}/ota/progress")
def api_ota_progress(ip: str):
    """Polled by the browser while an /ota/upload request for this same
    IP is in flight on another thread -- see the comment above."""
    return _ota_progress.get(ip, {"state": "idle", "bytes_sent": 0, "total_bytes": 0, "message": ""})


# ======================================================================= #
# Frontend (single static page, vanilla JS -- see static/index.html)
# ======================================================================= #
@app.get("/")
def index():
    return FileResponse(_STATIC_DIR / "index.html")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("webui.app:app", host="127.0.0.1", port=8000, reload=False)
