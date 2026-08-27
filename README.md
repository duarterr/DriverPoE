# DriverPoE

Firmware for a PoE-powered LED luminaire. It runs on an ESP32, gets power through a TPS2378, drives an HV9910 LED driver, and connects to the network over Ethernet through an IP101G PHY.

## What it does

- Confirms PoE or auxiliary power is stable before releasing the LED.
- Measures bus voltage and LED voltage.
- Turns on, turns off, and dims with a smooth ramp.
- Saves the last on/off state and last brightness to NVS.
- Gets an IP over Ethernet DHCP, announcing itself with the hostname "DriverPoE" (instead of ESP-IDF's default "espressif") in the router's client list.
- Allows local and remote control over an authenticated UDP channel (`tools/lumtool.py`, or the web UI in `tools/webui/`).
- Receives firmware updates (OTA) over that same administrative UDP channel, with SHA-256 verification of the image before applying it.

The product is Ethernet/PoE only: no Wi-Fi, no Bluetooth, no Matter. The only control protocol is the custom administrative UDP channel described below.

## How it works

On boot, the LED driver starts off. The firmware watches the TPS2378's signals and VBUS voltage. It only considers power valid once a source is detected and the bus is above 40 V. If power drops, it disables the driver immediately.

Ethernet only starts once power is confirmed; from there the firmware starts the administrative UDP channel. If the LED was on before a restart, it turns back on (at the same brightness) as soon as power is confirmed — including when that "turn on" was requested remotely while power wasn't ready yet (see "Administrative channel" below). `INFO` always reports the exact reason for a pending block (CDB/T2P/VBUS), even though today it's only reachable once power has already been confirmed.

| Indicator | GPIO | Meaning |
| --- | --- | --- |
| Blue | GPIO12 | Blinks while power hasn't been confirmed; stays on once VBUS is valid. |
| Red | GPIO14 | On while the HV9910 driver is enabled. |

## Administrative channel

Custom binary protocol over UDP, port `5001` (`ADMIN_UDP_PORT`), version `4` (`ADMIN_PROTO_VERSION` in `components/admin_channel/admin_protocol.h`).

**Security model**: each unit has a 32-byte secret (`components/devid/devid.h`), written to NVS automatically with a documented factory-default value (`ADMIN_DEFAULT_SECRET`, in `main/poe_luminaire_main.h`) on first boot — and again after any `FACTORY_RESET`, which erases the same NVS partition the secret lives in. `INFO` is the only unauthenticated command (pure read, no side effect, answered to anyone — including broadcast). Every command that changes something (`ON`/`OFF`/`DIM`/`IDENTIFY`/`REBOOT`/`FACTORY_RESET`/`CHANGE_SECRET`) requires HMAC-SHA256 with the active secret **and** a single-use nonce obtained via `CHALLENGE` immediately before, bound to the source IP and short-lived (5 s) — this protects against replaying a captured packet, not just forgery. `CHANGE_SECRET` swaps the secret directly (payload encrypted with AES-256-GCM under the current secret), with no separate confirmation step.

The default secret is known (it's in this repository) — this layer's real security comes from changing it during installation, not from keeping it secret. Treat it like the default password printed on the bottom of a home router.

Each device also rate-limits incoming packets to 20 per second per source IP (`admin_channel.c`, `RATE_LIMIT_MAX_PER_WINDOW`); anything past that is silently dropped, with no response at all — indistinguishable on the wire from the packet never arriving. A client that dims or sends other write commands at a high, sustained rate (a lighting effect, a music-reactive mode) needs to stay comfortably under this, and should expect an occasional dropped update to be normal rather than a bug.

**`ON`/`DIM` semantics**: the admin channel only starts listening after power has already been confirmed, so in practice this is rarely hit — but if an `ON` or `DIM>0` arrives while `tps2378_is_ready()` is false (e.g. power dropped and hasn't returned yet), the command is accepted and the intent is persisted anyway (never turning the driver on outside the TPS2378's electrical gate) — the response uses the `ACCEPTED_PENDING` status, distinct from `OK`, to make that explicit to the caller. The LED turns on by itself, at the requested brightness, as soon as power is confirmed (or reconfirmed). `OFF`/`DIM 0` always apply and normally persist immediately, regardless of power state — see the ramp/persistence notes below for the two ways that's more nuanced than it sounds.

**`DIM`'s ramp and the "transient" flag**: `DIM`'s payload is `percent` (1 byte) + `ramp_ms` (4 bytes, big-endian), with an optional 6th byte since this session's changes: a flags byte whose bit 0 means "transient". A 5-byte payload (every client before this addition, and still the default from `tools/device_api/client.py`'s `dim()`) always persists the new brightness to NVS as the resume value, exactly as before. Setting the transient bit skips that NVS write entirely — meant for a caller that dims at several times a second (an effect, a music-reactive mode): persisting every single one of those would mean a blocking flash commit on the same task that also drains the device's own command queue, and enough of them in a row makes the queue back up, so the driver's actual brightness visibly lags further and further behind what was just requested. `tools/webui_demo` sets this flag for every dim its effects and reactive-to-sound mode send; a plain slider drag from `tools/webui` does not, since that's a deliberate, infrequent brightness choice worth remembering.

Dimming to 0 with a nonzero ramp doesn't cut power the instant the command is received — it fades the PWM duty down first, and only asserts the driver's hardware SHUTDOWN pin once that fade genuinely finishes on the LEDC peripheral (a hardware fade-completion callback confirms this, not a wall-clock guess), so a fixture commanded to fade out and immediately commanded to something else again mid-fade never gets stuck partway between the two.

`FACTORY_RESET` erases the unit's entire NVS partition: saved brightness/on-off state and the administrative secret (which reverts to the factory default).

### Host tools

All of the protocol/HMAC/AES-GCM/discovery logic lives in a single pure Python package, `tools/device_api/` (no `input()`/`print()`/side effects outside the network) — `tools/lumtool.py` and `tools/webui/` are consumers of that package, not parallel reimplementations of it.

**`tools/device_api/`** — Python API:

| Module | Contents |
| --- | --- |
| `protocol.py` | Constants, `Packet` (serialization/HMAC), identity (MAC/serial), `parse_info_payload()`. |
| `client.py` | `AdminClient` (one UDP socket per unit; `info/challenge/on/off/dim/identify/reboot/factory_reset/change_secret`), typed exceptions (`DeviceTimeoutError`, `AuthError`, `ProtocolError`/`ProtocolVersionMismatchError`, `CommandRefusedError`, `MissingDependencyError`), `find_working_secret()`/`connect()`. |
| `discovery.py` | `broadcast_info()`, `resolve_device_by_ip()`, `guess_broadcast_address()`. |
| `models.py` | `DeviceInfo` (with `power_blocking_reason`), `CommandResult` (`accepted`/`applied`/`pending`, `raise_if_refused()`). |
| `secrets.py` | `SecretStore` (minimal protocol: `get/set/delete`), `JsonFileSecretStore` (`tools/admin_secrets.json`, gitignored), `MemorySecretStore`. |
| `cli.py` | The interactive menu — the only place in the package with terminal I/O. |

```powershell
python -c "from device_api import discovery, connect, JsonFileSecretStore; d=discovery.broadcast_info(discovery.guess_broadcast_address()); print(d)"
```

**`tools/lumtool.py`** — thin CLI over the package above (interactive menu: scan, select, control, administer):

```powershell
python tools/lumtool.py
```

**`tools/webui/`** — local web UI (FastAPI + plain HTML/JS, no frontend framework), consuming the `device_api` package exclusively on the backend; the admin secret is never sent to the browser except once, back to the operator, right after a `CHANGE_SECRET` they themselves requested:

```powershell
pip install -r tools/webui/requirements.txt
Set-Location tools
python -m webui.app   # or: uvicorn webui.app:app --reload
```

Open `http://127.0.0.1:8000/`. It's a local tool with no authentication of its own (same posture as the UDP channel: anyone who can reach the UI can send authenticated commands) — don't expose it outside a trusted management network.

**`tools/webui_demo/`** — a standalone demo UI for presenting the network as a stage: group commands, dimmer, one-fixture-at-a-time identify, pulse/wave scenes, and brightness reactive to a local audio file. Reuses the `device_api` package and the same local secret storage as the admin WebUI:

```powershell
pip install -r tools/webui_demo/requirements.txt
Set-Location tools
python -m webui_demo.app
```

Open `http://127.0.0.1:8001/`; keep it on a trusted management network.

The reactive-to-sound mode analyzes the whole picked file server-side, once, before playback starts (`tools/webui_demo/audio_reactive.py`: a mel-spaced filterbank, per-band automatic gain, spectral-flux onset/beat detection, and an asymmetric attack/release envelope — see that module's own docstring for the algorithm) — the browser just decodes the file to raw samples, posts them to `/api/music/analyze`, and during playback looks up the precomputed intensity for the current position instead of analyzing anything live.

## Board configuration

Hardware-specific parameters live in [main/poe_luminaire_main.h](main/poe_luminaire_main.h): GPIOs, polarities, PHY address, voltage divider, VBUS threshold, and UDP port. Adjust that file before building for a different hardware revision. `DEVID_MODEL_PREFIX` (currently "DriverPoE") is the single source of the product name — it prefixes both the serial (`devid_get_serial()`) and the hostname announced over DHCP (`eth_init`'s `hostname` config); changing that `#define` updates both automatically.

This board's main configuration:

| Item | Value |
| --- | --- |
| MCU | ESP32 |
| Ethernet | Internal EMAC, RMII, IP101G PHY (address 1) |
| PoE | TPS2378; IEEE 802.3af/at or auxiliary power |
| LED driver | HV9910, 10 kHz PWM |
| Minimum VBUS | 40 V |
| Flash | 4 MB, two OTA partitions |

## Build and flash

The project uses ESP-IDF `6.0.2`. The `ip101` dependency (PHY driver) is downloaded by the Component Manager on first build. The ESP-IDF environment must be loaded.

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM_X flash monitor
```

Replace `COM_X` with the board's serial port.

To run the host tools' test suite (protocol, client, discovery, models, secrets):

```powershell
Set-Location tools
python -m unittest discover -s device_api/tests -v
```

## Structure

| Path | Responsibility |
| --- | --- |
| `main/` | Component initialization and integration. |
| `components/hv9910/` | LED driver brightness and state control. |
| `components/tps2378/` | PoE/AUX detection and power validation. |
| `components/voltage_sense/` | VBUS and LED voltage readings via ADC. |
| `components/eth_init/` | RMII Ethernet and DHCP. |
| `components/admin_channel/` | Authenticated UDP protocol. |
| `components/devid/` | Serial, MAC, and administrative secret. |
| `components/status_leds/` | Board status LEDs. |
| `tools/device_api/` | Python package: protocol, client, discovery, models, secrets. |
| `tools/lumtool.py` | Discovery/control/admin CLI, built on `tools/device_api/`. |
| `tools/webui/` | Local web UI (FastAPI), built on `tools/device_api/`. |

## OTA update

The image is transferred over the **same administrative UDP channel** (`ADMIN_TYPE_OTA_BEGIN/OTA_CHUNK/OTA_END/OTA_ABORT`, protocol version `4`) — no HTTP client in the firmware, no new network surface. Flow:

1. `OTA_BEGIN` (HMAC + nonce) announces the total size and the SHA-256 of the complete image.
2. `OTA_CHUNK` (HMAC, no nonce — see the `needs_pool_nonce` comment in `admin_channel.c` for why) sends the image in chunks of up to 1024 bytes, each acknowledged with the total written so far — losing a response is safe, the client just resends the same chunk.
3. `OTA_END` (HMAC + nonce) checks that every byte arrived, verifies the SHA-256 of the whole image, and only then validates it (`esp_ota_end`) and marks the partition as next to boot. Any failure aborts without touching the current partition.
4. The new image only ever runs after an explicit `REBOOT` — never automatically. From there, the existing rollback confirmation (`confirm_app_if_pending_verify()` in `main.c` + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) guarantees that an image that doesn't confirm itself reverts to the previous one.

A stalled OTA session (no `OTA_CHUNK` for 30s) is discarded automatically, so an abandoned transfer never permanently locks the channel for a later attempt.

**From the web UI**: each card has a file field + an "Upload firmware" button — pick the `.bin` (e.g. `build/driverpoe.bin`) and send it; a progress bar tracks the transfer in real time, and once it's done the page offers to reboot the unit to apply it.

**From the Python package**: `AdminClient.ota_update(secret, serial, image_bytes, progress_callback=...)`, in `tools/device_api/client.py`.
