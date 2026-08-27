"""Check whether DriverPoE units' admin channel is actually protected.

Run from ``tools/``:

    python audit_admin.py                 # scan the LAN, check every unit
    python audit_admin.py 192.168.0.40    # check one unit

For each unit it reports:
  * reachable?                    (unauthenticated INFO)
  * factory-default secret works? -> if YES the unit is NOT protected
  * a wrong random secret works?  -> must always be NO (proves the
                                     firmware is enforcing the HMAC, not
                                     just answering everyone)
  * vault secret present & works? (only if a vault exists / is unlocked)

Nothing here changes any device state -- it only does INFO + CHALLENGE.
"""
from __future__ import annotations

import getpass
import os
import sys

from device_api import ADMIN_DEFAULT_SECRET, AdminClient, default_vault_path, discovery
from device_api.client import AuthError, DeviceTimeoutError
from device_api.secrets import EncryptedFileSecretStore, VaultError

GREEN, RED, YELLOW, DIM, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"


def _challenge_ok(client: AdminClient, secret: bytes, serial: str) -> bool:
    """True if `secret` is the unit's active admin secret. A wrong secret
    gets no CHALLENGE_RESP at all (the firmware silently drops any packet
    whose HMAC doesn't verify), so that surfaces here as a timeout."""
    try:
        client.challenge(secret, serial)
        return True
    except (AuthError, DeviceTimeoutError):
        return False


def _open_vault():
    path = default_vault_path()
    if not path.exists():
        return None
    pw = os.environ.get("DRIVERPOE_VAULT_PASSPHRASE") or getpass.getpass(f"Vault passphrase [{path}] (Enter to skip): ")
    if not pw:
        return None
    try:
        return EncryptedFileSecretStore(path, pw)
    except VaultError as e:
        print(f"{YELLOW}vault: {e}{RESET}")
        return None


def audit(ip: str, vault) -> None:
    client = AdminClient(ip, timeout=2.0)
    try:
        info = client.info()
    except Exception as e:
        print(f"{ip}: {RED}unreachable{RESET} ({type(e).__name__})")
        return

    default_works = _challenge_ok(client, ADMIN_DEFAULT_SECRET, info.serial)
    wrong_works = _challenge_ok(client, bytes(range(1, 33)), info.serial)  # arbitrary non-default

    if default_works:
        verdict = f"{RED}NOT PROTECTED{RESET}  -- still using the factory-default secret"
    elif wrong_works:
        verdict = f"{RED}BROKEN{RESET}  -- a wrong secret was accepted (firmware not enforcing HMAC!)"
    else:
        verdict = f"{GREEN}protected{RESET}  -- a per-unit secret is set and enforced"

    line = f"{info.serial} @ {ip}: {verdict}"

    if vault is not None:
        s = vault.get(info.serial)
        if s is None:
            line += f"  {DIM}(no secret in the vault for this unit){RESET}"
        elif _challenge_ok(client, s, info.serial):
            line += f"  {GREEN}vault secret works{RESET}"
        else:
            line += f"  {RED}vault secret MISMATCH{RESET}"
    print(line)
    client.close()


def main() -> None:
    vault = _open_vault()
    if len(sys.argv) > 1:
        ips = sys.argv[1:]
    else:
        print("Scanning the LAN...")
        found = discovery.broadcast_info(discovery.guess_broadcast_address())
        ips = [d.source_ip for d in sorted(found, key=lambda d: d.serial)]
        if not ips:
            raise SystemExit("No unit responded to the broadcast.")
    for ip in ips:
        audit(ip, vault)


if __name__ == "__main__":
    main()
