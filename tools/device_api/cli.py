"""Interactive command-line interface, built entirely on top of the
device_api package -- no protocol/HMAC/socket code of its own. This is
where every input()/print()/getpass()/open() in this package lives;
nothing above this module does interactive or disk I/O.

    python -m device_api.cli [keys.txt]

Flow: discovery -> pick a unit -> per-device menu, with an explicit
confirmation on every destructive action.
"""
from __future__ import annotations

import getpass
import os
import socket
import sys

from . import discovery
from .client import (
    AdminClient,
    CommandRefusedError,
    DeviceTimeoutError,
    DriverPoEError,
    connect,
)
from .models import DeviceInfo
from .protocol import (
    DEFAULT_PORT,
    DEFAULT_RAMP_MS,
    DEFAULT_TIMEOUT,
    DMX_LOSS_NAMES,
    DMX_MERGE_NAMES,
    DMX_PERSONALITY_NAMES,
    SECRET_LEN,
    DmxConfig,
)
from .secrets import KeyfileSecretStore, KeysFileError, MemorySecretStore, SecretStore


def _prompt_admin_secret(serial: str) -> bytes:
    """The manual_secret_provider passed to find_working_secret() when
    neither a saved nor the factory-default secret works -- the only
    place this CLI asks for a secret via a hidden prompt instead of a
    command-line argument (which would end up in shell history / `ps`)."""
    typed = getpass.getpass(f"Admin secret for {serial} (64 hex chars, input hidden): ").strip()
    try:
        secret = bytes.fromhex(typed)
    except ValueError:
        raise SystemExit("Secret must be a 64-character hex string.")
    if len(secret) != SECRET_LEN:
        raise SystemExit(f"Secret must be {SECRET_LEN} bytes; got {len(secret)}.")
    return secret


def _connect(ip: str, store: SecretStore, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT):
    try:
        return connect(ip, store, manual_secret_provider=lambda: _prompt_admin_secret("this unit"),
                        port=port, timeout=timeout)
    except DeviceTimeoutError:
        raise SystemExit("Timed out waiting for the unit's response -- right IP/port? Unit powered and reachable?")


# ======================================================================= #
# Printing
# ======================================================================= #
def print_info(info: DeviceInfo) -> None:
    print(f"Serial:               {info.serial}")
    print(f"MAC:                  {info.mac_str}")
    print(f"Firmware:             {info.fw_version}")
    print(f"IP:                   {info.ip}  (responded from {info.source_ip})")
    print(f"Uptime:               {info.uptime_s}s")
    print(f"Reset reason:         {info.reset_reason}")
    print(f"PoE ready:            {info.poe_ready} (source: {info.poe_source})")
    if not info.poe_ready:
        print(f"  Blocked on:          {info.power_blocking_reason}")
        print(f"  CDB/T2P/VBUS ok:     {info.poe_cdb_confirmed}/{info.poe_t2p_confirmed}/{info.poe_vbus_confirmed}")
    print(f"Driver on (actual):   {info.driver_on} (dim={info.dim_percent}%)")
    print(f"Driver on (desired):  {info.desired_on}{'  <- pending, will apply once power is confirmed' if info.desired_on and not info.driver_on else ''}")
    if info.ramp_pending:
        print("Ramp/blink in progress.")
    print(f"VBUS:                 {info.vbus_mv}mV")
    print(f"LED voltage:          {info.led_voltage_mv}mV")
    print(f"DMX layer:            {'enabled' if info.dmx_layer_enabled else 'disabled'}"
          f"  (live source: {info.dmx_active_source}, {info.dmx_level}% @ {info.dmx_fps}fps)")
    if info.dmx_layer_enabled:
        print(f"  Art-Net port-addr:   0x{info.dmx_artnet_port_address:04x}   sACN universe: {info.dmx_sacn_universe}")
        if info.dmx_last_src_ip not in ("", "0.0.0.0"):
            print(f"  Last DMX source IP:  {info.dmx_last_src_ip}")


def print_scan_results(results: list[DeviceInfo]) -> None:
    if not results:
        print("No unit responded.")
        return
    for i, info in enumerate(results, 1):
        ready = "power ready" if info.poe_ready else f"NOT ready ({info.power_blocking_reason})"
        print(f"  [{i}] {info.serial}  ip={info.source_ip}  fw={info.fw_version}  ({ready})")


# ======================================================================= #
# Actions -- each takes (ip, serial, store) and does exactly one thing,
# printing its own result/error. Never raises past this point (see
# run_action() below, which is the single place that catches everything).
# ======================================================================= #
def action_info(ip: str, serial: str, store: SecretStore) -> None:
    with AdminClient(ip) as client:
        print_info(client.info())


def action_identify(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        result = client.identify(secret, info.serial)
        if result.applied:
            print(f"{info.serial}: blinking for a few seconds.")
        else:
            print(f"{info.serial}: IDENTIFY refused ({result.status.name}).")


def _prompt_ramp_ms() -> int:
    raw = input(f"Ramp time in ms [{DEFAULT_RAMP_MS}]: ").strip()
    return int(raw) if raw else DEFAULT_RAMP_MS


def action_on(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        result = client.on(secret, info.serial, _prompt_ramp_ms())
        if result.applied:
            print(f"{info.serial}: turning on.")
        elif result.pending:
            print(f"{info.serial}: accepted -- will turn on once power is confirmed (not ready yet).")
        else:
            print(f"{info.serial}: ON refused ({result.status.name}).")


def action_off(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        result = client.off(secret, info.serial, _prompt_ramp_ms())
        if result.applied:
            print(f"{info.serial}: turning off.")
        else:
            print(f"{info.serial}: OFF refused ({result.status.name}).")


def action_dim(ip: str, serial: str, store: SecretStore) -> None:
    percent_raw = input("Brightness percent (0-100): ").strip()
    if not percent_raw:
        print("Percent is required.")
        return
    try:
        percent = int(percent_raw)
    except ValueError:
        print("Percent must be an integer.")
        return
    client, info, secret = _connect(ip, store)
    with client:
        try:
            result = client.dim(secret, info.serial, percent, _prompt_ramp_ms())
        except ValueError as e:
            print(f"Error: {e}")
            return
        if result.applied:
            print(f"{info.serial}: dimming to {percent}%.")
        elif result.pending:
            print(f"{info.serial}: accepted -- will apply {percent}% once power is confirmed (not ready yet).")
        else:
            print(f"{info.serial}: DIM refused ({result.status.name}).")


def action_reboot(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        result = client.reboot(secret, info.serial)
        if result.applied:
            print(f"{info.serial}: reboot confirmed by the device.")
        else:
            print(f"{info.serial}: REBOOT refused ({result.status.name}).")


def action_reset(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        typed = input(f"Type the exact serial to confirm the FACTORY_RESET ({info.serial}): ")
        if typed.strip() != info.serial:
            print("Serial doesn't match -- aborted, nothing was sent.")
            return
        result = client.factory_reset(secret, info.serial)
        if not result.applied:
            print(f"{info.serial}: FACTORY_RESET refused ({result.status.name}).")
            return
        # The device reverts its admin secret to the factory default as
        # part of a FACTORY_RESET -- a locally saved custom secret for
        # this serial is now stale.
        store.delete(info.serial)
        print(f"{info.serial}: FACTORY_RESET confirmed -- brightness memory and admin secret "
              f"reverted to factory defaults.")


def action_change_secret(ip: str, serial: str, store: SecretStore) -> None:
    choice = input("New secret: [R]andom (recommended) or [T]ype one in? [R]: ").strip().lower()
    new_secret = None
    if choice == "t":
        typed = getpass.getpass("New admin secret (64 hex chars, input hidden): ").strip()
        try:
            new_secret = bytes.fromhex(typed)
        except ValueError:
            print("Secret must be a 64-character hex string.")
            return
        if len(new_secret) != SECRET_LEN:
            print(f"Secret must be {SECRET_LEN} bytes; got {len(new_secret)}.")
            return

    client, info, secret = _connect(ip, store)
    with client:
        result, applied_secret = client.change_secret(secret, info.serial, new_secret)
        if not result.applied:
            print(f"{info.serial}: CHANGE_SECRET refused ({result.status.name}).")
            return
        store.set(info.serial, applied_secret)
        print(f"{info.serial}: admin secret changed.")
        print(f"New secret: {applied_secret.hex()}")
        print(f"Saved to {getattr(store, 'path', '(in-memory store)')} -- this tool will use it automatically from now on.")


def _prompt_int(label: str, current: int, lo: int, hi: int) -> int:
    raw = input(f"  {label} [{current}]: ").strip()
    if not raw:
        return current
    try:
        v = int(raw, 0)
    except ValueError:
        print("  Not a number -- keeping the current value.")
        return current
    return max(lo, min(hi, v))


def _prompt_bool(label: str, current: bool) -> bool:
    raw = input(f"  {label} [{'y' if current else 'n'}]: ").strip().lower()
    if not raw:
        return current
    return raw in ("y", "yes", "1", "true")


def _print_dmx_config(cfg: DmxConfig) -> None:
    protos = []
    if cfg.proto_mask & 0x01:
        protos.append("Art-Net")
    if cfg.proto_mask & 0x02:
        protos.append("sACN")
    print(f"  Layer enabled:       {cfg.layer_enabled}")
    print(f"  Protocols:           {', '.join(protos) or 'none'}")
    print(f"  Art-Net universe:    net {cfg.artnet_net} / sub {cfg.artnet_subnet} / uni {cfg.artnet_universe}"
          f"  (port-address 0x{cfg.artnet_port_address:04x})")
    print(f"  sACN universe:       {cfg.sacn_universe}")
    print(f"  DMX start address:   {cfg.dmx_address}")
    print(f"  Personality:         {DMX_PERSONALITY_NAMES.get(cfg.personality, cfg.personality)}")
    print(f"  Merge mode:          {DMX_MERGE_NAMES.get(cfg.merge_mode, cfg.merge_mode)}")
    print(f"  Signal-loss:         {DMX_LOSS_NAMES.get(cfg.loss_behavior, cfg.loss_behavior)}"
          f" (level {cfg.loss_level}%, timeout {cfg.loss_timeout_ms}ms)")
    print(f"  Inter-frame smooth:  {cfg.smoothing_ms}ms")
    print(f"  Accept ArtAddress:   {cfg.allow_artaddress}")


def action_dmx_show(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        print(f"\n{info.serial}: DMX layer configuration")
        _print_dmx_config(client.get_dmx_config(secret, info.serial))


def action_dmx_configure(ip: str, serial: str, store: SecretStore) -> None:
    client, info, secret = _connect(ip, store)
    with client:
        cfg = client.get_dmx_config(secret, info.serial)
        print(f"\n{info.serial}: edit each field (Enter keeps the current value).")
        cfg.layer_enabled = _prompt_bool("Enable the DMX layer?", cfg.layer_enabled)
        artnet = _prompt_bool("Receive Art-Net?", bool(cfg.proto_mask & 0x01))
        sacn = _prompt_bool("Receive sACN?", bool(cfg.proto_mask & 0x02))
        cfg.proto_mask = (0x01 if artnet else 0) | (0x02 if sacn else 0)
        net = _prompt_int("Art-Net Net (0-127)", cfg.artnet_net, 0, 127)
        sub = _prompt_int("Art-Net Sub-Net (0-15)", cfg.artnet_subnet, 0, 15)
        uni = _prompt_int("Art-Net Universe (0-15)", cfg.artnet_universe, 0, 15)
        cfg.artnet_port_address = DmxConfig.from_artnet_parts(net, sub, uni)
        cfg.sacn_universe = _prompt_int("sACN universe (1-63999)", cfg.sacn_universe, 1, 63999)
        cfg.dmx_address = _prompt_int("DMX start address (1-512)", cfg.dmx_address, 1, 512)
        cfg.personality = _prompt_int("Personality (0=1ch 8-bit, 1=2ch 16-bit)", cfg.personality, 0, 1)
        cfg.merge_mode = _prompt_int("Merge mode (0=HTP, 1=LTP)", cfg.merge_mode, 0, 1)
        cfg.loss_behavior = _prompt_int("On signal loss (0=hold, 1=to-black, 2=to-level)", cfg.loss_behavior, 0, 2)
        cfg.loss_level = _prompt_int("Loss level % (for to-level)", cfg.loss_level, 0, 100)
        cfg.loss_timeout_ms = _prompt_int("Loss timeout ms", cfg.loss_timeout_ms, 500, 60000)
        cfg.smoothing_ms = _prompt_int("Inter-frame smoothing ms", cfg.smoothing_ms, 0, 5000)
        cfg.allow_artaddress = _prompt_bool("Accept ArtAddress from the network?", cfg.allow_artaddress)

        result = client.set_dmx_config(secret, info.serial, cfg)
        if not result.applied:
            print(f"{info.serial}: DMX_SET_CONFIG refused ({result.status.name}).")
            return
        print(f"{info.serial}: DMX config saved. Device now reports:")
        _print_dmx_config(client.get_dmx_config(secret, info.serial))


def run_action(handler, ip: str, serial: str, store: SecretStore) -> None:
    """Runs one action_* handler against the selected device, keeping the
    menu alive across every error class the package defines (plus
    KeyboardInterrupt and, as a last resort, anything unexpected)."""
    try:
        handler(ip, serial, store)
    except (DeviceTimeoutError, socket.timeout):
        print("Timed out waiting for the unit's response -- right IP/port? Unit powered and reachable?")
    except CommandRefusedError as e:
        print(f"Error: {e}")
    except (DriverPoEError, SystemExit) as e:
        print(f"Error: {e}")
    except KeyboardInterrupt:
        print("\nCancelled.")
    except Exception as e:  # last resort -- keep the menu alive
        print(f"Unexpected error: {e}")


# ======================================================================= #
# Interactive menu -- scan first, pick a luminaire from the list, then get
# a device-specific menu with all its options (diskpart-style: "list" ->
# "select" -> act on the selected item). Every unit is administrable from
# the start (factory-default secret) -- there's no "unprovisioned" state.
# ======================================================================= #
def scan_devices() -> list[DeviceInfo]:
    default_bcast = discovery.guess_broadcast_address()
    broadcast = input(f"Broadcast address [{default_bcast}]: ").strip() or default_bcast
    print("Scanning...")
    try:
        results = discovery.broadcast_info(broadcast, DEFAULT_PORT, DEFAULT_TIMEOUT)
    except OSError as e:
        print(f"Broadcast failed: {e}")
        return []
    results.sort(key=lambda info: info.serial)
    print_scan_results(results)
    return results


INFO_CONTROL_MENU = {
    "1": ("Info", action_info),
    "2": ("Identify (blink)", action_identify),
    "3": ("On", action_on),
    "4": ("Off", action_off),
    "5": ("Dim", action_dim),
}
ADMINISTRATION_MENU = {
    "1": ("Reboot", action_reboot),
    "2": ("Factory reset", action_reset),
    "3": ("Change admin secret", action_change_secret),
}
DMX_MENU = {
    "1": ("Show DMX config", action_dmx_show),
    "2": ("Configure DMX / Art-Net / sACN", action_dmx_configure),
}


def run_submenu(title: str, items: dict, ip: str, serial: str, store: SecretStore) -> None:
    while True:
        print(f"\n  -- {title} --")
        for key, (label, _) in items.items():
            print(f"    {key}) {label}")
        print("    0) Back")
        choice = input("  > ").strip()
        if choice in ("0", ""):
            return
        if choice not in items:
            print("  Invalid choice.")
            continue
        _, handler = items[choice]
        run_action(handler, ip, serial, store)


def device_menu(serial: str, ip: str, store: SecretStore) -> None:
    while True:
        print(f"\n=== {serial}  ip={ip} ===")
        print("  1) Info & control  (info / identify / on / off / dim)")
        print("  2) Administration  (reboot / factory reset / change admin secret)")
        print("  3) DMX / Art-Net / sACN  (show / configure)")
        print("  0) Back to device list")
        choice = input("> ").strip()
        if choice in ("0", ""):
            return
        if choice == "1":
            run_submenu("Info & control", INFO_CONTROL_MENU, ip, serial, store)
        elif choice == "2":
            run_submenu("Administration", ADMINISTRATION_MENU, ip, serial, store)
        elif choice == "3":
            run_submenu("DMX / Art-Net / sACN", DMX_MENU, ip, serial, store)
        else:
            print("Invalid choice.")


def _open_store() -> SecretStore:
    """Load an admin keys file if one was given (first CLI arg, or
    $DRIVERPOE_KEYS) -- held in RAM only, never written. Otherwise a bare
    RAM store where each unit's secret is typed per session."""
    path = None
    if len(sys.argv) > 1:
        path = sys.argv[1]
    elif os.environ.get("DRIVERPOE_KEYS"):
        path = os.environ["DRIVERPOE_KEYS"]
    if not path:
        return MemorySecretStore()
    try:
        text = open(path, encoding="utf-8").read()
        store = KeyfileSecretStore()
        store.load_text(text)
    except (OSError, KeysFileError) as e:
        raise SystemExit(f"keys file: {e}")
    s = store.stats()
    print(f"Loaded {s['count']} key(s) from {path}"
          + (f" + {s['fallback_count']} fallback(s)" if s["fallback_count"] else "")
          + " -- held in memory only.")
    return store


def main() -> None:
    print("=== DriverPoE admin tool ===")
    store = _open_store()
    last_scan: list[DeviceInfo] = []
    while True:
        print()
        if last_scan:
            print_scan_results(last_scan)
        else:
            print("(no scan yet)")
        print("  [S] Scan the network")
        print("  [M] Enter a device IP manually")
        print("  [Q] Quit")
        choice = input("> ").strip()
        lowered = choice.lower()

        if choice == "" or lowered == "q":
            break
        if lowered == "s":
            last_scan = scan_devices()
            continue
        if lowered == "m":
            ip = input("IP: ").strip()
            if not ip:
                continue
            info = discovery.resolve_device_by_ip(ip)
            if info is None:
                print("No response from that IP.")
                continue
            device_menu(info.serial, ip, store)
            continue

        try:
            idx = int(choice)
        except ValueError:
            print("Invalid choice.")
            continue
        if not last_scan or not (1 <= idx <= len(last_scan)):
            print("Invalid choice.")
            continue
        info = last_scan[idx - 1]
        device_menu(info.serial, info.source_ip, store)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye.")
