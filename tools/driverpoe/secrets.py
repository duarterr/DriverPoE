"""Admin secret storage -- the factory default, plus a small, swappable
storage interface for per-unit secrets an operator has changed away from
it (TODO Fase 3: "separate/swappable secret storage layer").

Two implementations ship here: JsonFileSecretStore (what lumtool.py has
always used -- a local plaintext JSON file) and MemorySecretStore (no
persistence at all -- useful for tests, and for any caller, like a
short-lived script, that shouldn't touch disk). A web backend that wants a
different backing store (a real secrets manager, an encrypted-at-rest
file, ...) implements the same three-method SecretStore protocol.
"""
from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from typing import Protocol

from .protocol import SECRET_LEN

# MUST match ADMIN_DEFAULT_SECRET in main/poe_luminaire_main.h exactly --
# same 32 ASCII bytes, spelled out there as hex only so the array's exact
# length is unambiguous at a glance. Documented in the clear on purpose
# (same as a home router's printed default password) -- it's a known
# starting point, not a real secret by itself; see README.md "Canal de
# administração" for why that's fine, and why changing it during
# installation is what actually matters.
ADMIN_DEFAULT_SECRET = b"DriverPoE-default-admin-secret!!"
assert len(ADMIN_DEFAULT_SECRET) == SECRET_LEN, len(ADMIN_DEFAULT_SECRET)

DEFAULT_SECRETS_FILE = Path(__file__).resolve().parent.parent / "admin_secrets.json"


class SecretStore(Protocol):
    """Minimal interface client.py's find_working_secret() (and any
    caller) needs -- serial -> secret, nothing more. Implementations
    decide their own persistence (or lack of it)."""

    def get(self, serial: str) -> bytes | None: ...
    def set(self, serial: str, secret: bytes) -> None: ...
    def delete(self, serial: str) -> None: ...


class MemorySecretStore:
    """No persistence -- lives only as long as the process. Useful for
    tests and for short-lived scripts that shouldn't write to disk."""

    def __init__(self, initial: dict[str, bytes] | None = None) -> None:
        self._store: dict[str, bytes] = dict(initial or {})

    def get(self, serial: str) -> bytes | None:
        return self._store.get(serial)

    def set(self, serial: str, secret: bytes) -> None:
        if len(secret) != SECRET_LEN:
            raise ValueError(f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
        self._store[serial] = secret

    def delete(self, serial: str) -> None:
        self._store.pop(serial, None)

    def as_dict(self) -> dict[str, bytes]:
        """Snapshot -- for tests/inspection only, not part of the
        SecretStore protocol."""
        return dict(self._store)


class JsonFileSecretStore:
    """serial -> secret, persisted as a small plaintext JSON file (hex
    strings). This file holds every per-unit secret an operator has ever
    changed away from the factory default -- protect the machine that
    runs this accordingly (same posture the old lumtool.py's
    admin_secrets.json always had; see .gitignore, which excludes this
    file's default path by name).

    Re-reads the file on every get() (cheap -- this is a tiny file used at
    human/CLI speed, never a hot path) so multiple short-lived processes
    (e.g. repeated `lumtool.py` CLI invocations) always see each other's
    changes without needing to share a single long-lived instance.
    """

    def __init__(self, path: Path | str = DEFAULT_SECRETS_FILE) -> None:
        self.path = Path(path)

    def _load(self) -> dict[str, bytes]:
        if not self.path.exists():
            return {}
        try:
            raw = json.loads(self.path.read_text())
        except (OSError, ValueError):
            # Missing/unreadable/corrupt file just means "nothing saved
            # yet" -- never fatal, the factory default and manual entry
            # are always available as fallbacks (see client.py's
            # find_working_secret()).
            return {}
        store: dict[str, bytes] = {}
        for serial, hex_secret in raw.items():
            try:
                secret = bytes.fromhex(hex_secret)
            except (ValueError, AttributeError):
                continue
            if len(secret) == SECRET_LEN:
                store[serial] = secret
        return store

    def _save(self, store: dict[str, bytes]) -> None:
        data = {serial: secret.hex() for serial, secret in store.items()}
        self.path.write_text(json.dumps(data, indent=2, sort_keys=True))
        if os.name == "posix":
            try:
                os.chmod(self.path, stat.S_IRUSR | stat.S_IWUSR)
            except OSError:
                pass  # best-effort -- not fatal if this fails

    def get(self, serial: str) -> bytes | None:
        return self._load().get(serial)

    def set(self, serial: str, secret: bytes) -> None:
        if len(secret) != SECRET_LEN:
            raise ValueError(f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
        store = self._load()
        store[serial] = secret
        self._save(store)

    def delete(self, serial: str) -> None:
        store = self._load()
        if serial in store:
            del store[serial]
            self._save(store)
