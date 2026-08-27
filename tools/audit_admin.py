"""Check whether DriverPoE units' admin channel is actually protected.

Run from ``tools/``:

    python audit_admin.py                       # scan the LAN
    python audit_admin.py 192.168.0.40          # check one unit
    python audit_admin.py --keys mykeys.txt     # try keys from a file too

For each unit it reports:
  * reachable?                    (unauthenticated INFO)
  * factory-default secret works? -> if YES the unit is NOT protected
  * a wrong random secret works?  -> must always be NO (proves the
                                     firmware is enforcing the HMAC, not
                                     just answering everyone)
  * keys-file key works?          (only with --keys)

Nothing here changes any device state -- it only does INFO + CHALLENGE.
"""
from __future__ import annotations

import sys

from device_api import ADMIN_DEFAULT_SECRET, AdminClient, KeyfileSecretStore, discovery
from device_api.client import AuthError, DeviceTimeoutError

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


def audit(ip: str, keychain: KeyfileSecretStore | None) -> None:
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

    if keychain is not None:
        cands = keychain.candidates(info.serial)
        if not cands:
            line += f"  {DIM}(no key in the file for this unit){RESET}"
        elif any(_challenge_ok(client, k, info.serial) for k in cands):
            line += f"  {GREEN}keys-file key works{RESET}"
        else:
            line += f"  {RED}no keys-file key works{RESET}"
    print(line)
    client.close()


def main() -> None:
    args = sys.argv[1:]
    keychain = None
    if "--keys" in args:
        i = args.index("--keys")
        keychain = KeyfileSecretStore()
        keychain.load_text(open(args[i + 1], encoding="utf-8").read())
        del args[i:i + 2]

    if args:
        ips = args
    else:
        print("Scanning the LAN...")
        found = discovery.broadcast_info(discovery.guess_broadcast_address())
        ips = [d.source_ip for d in sorted(found, key=lambda d: d.serial)]
        if not ips:
            raise SystemExit("No unit responded to the broadcast.")
    for ip in ips:
        audit(ip, keychain)


if __name__ == "__main__":
    main()
