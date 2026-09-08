# DriverPoE

A PoE-powered LED luminaire built around an ESP32: powered through a TPS2378,
driven by an HV9910 LED driver, on the network over Ethernet (IP101G PHY). It
turns on / off / dims over an authenticated UDP channel and takes **Art-Net**
and **sACN (E1.31)** once commissioned, so any lighting console drives it as a
1- or 2-channel DMX dimmer. Ethernet/PoE only — no Wi-Fi, no Bluetooth.

## Repository layout

| Path | What it is |
| --- | --- |
| [`firmware/`](firmware/) | The ESP-IDF firmware. Run `idf.py` from this directory. See [firmware/README.md](firmware/README.md) for the architecture, the admin protocol, the DMX layer, the dimming modes and the power policy. |
| [`tools/`](tools/) | Host-side Python. `device_api/` is the pure protocol/HMAC/AES-GCM/discovery library; `webui/` is the local admin web UI (FastAPI) built on it; `webui_demo/` is an Art-Net-only stage-control demo (no auth); `dmxtool/` is a stdlib-only Art-Net/sACN test transmitter. |
| [`tests/`](tests/) | Bench tool: an automated dimming/power sweep against real boards with a Rigol DP1308A PSU, a web UI plus a headless harness, and an `.xlsx` report. |
| [`manual/`](manual/) | End-user manual (`manual.html`). |

## Quick start

**Firmware** (ESP-IDF `6.0.2`, environment loaded):

```powershell
Set-Location firmware
idf.py set-target esp32
idf.py build
idf.py -p COM_X flash monitor
```

**Admin web UI:**

```powershell
pip install -r tools/webui/requirements.txt
Set-Location tools
python -m webui.app          # http://127.0.0.1:8000/
```

**Host-tool tests:**

```powershell
Set-Location tools
python -m unittest discover -s device_api/tests -v
python -m unittest discover -s dmxtool/tests -v
python -m unittest discover -s webui/tests -v         # needs fastapi
python -m unittest discover -s webui_demo/tests -v
```

## Two control planes

- **Administrative channel** — a custom authenticated binary protocol over UDP
  5001 (HMAC-SHA256 + single-use nonce, AES-256-GCM for the secret swap). Used
  for maintenance, commissioning and OTA. Each unit ships with a documented
  factory-default secret that is meant to be changed during installation.
- **DMX over Ethernet** — Art-Net (UDP 6454) and sACN (UDP 5568), unauthenticated
  by design exactly like physical DMX, disabled until commissioned. Keep the
  device on a trusted management/entertainment LAN.

Both are described in full in [firmware/README.md](firmware/README.md).
