"""Admin secret storage -- the factory default, plus a small, swappable
storage interface for per-unit secrets.

Nothing here is ever written to disk. The web UI / CLI hold keys in RAM
for the life of the process only:

- ``KeyfileSecretStore`` -- keys parsed from a plain-text file the
  operator supplies each session (``<serial or *>  <64 hex>`` per line).
  A ``*`` / ``any`` / ``all others`` line is the fallback key tried for
  serials not listed explicitly.
- ``MemorySecretStore`` -- a bare dict, filled per session (e.g. via
  client.py's ``find_working_secret(manual_secret_provider=...)``).

Anything else (an OS keychain, a real secrets manager) implements the
same three-method ``SecretStore`` protocol.
"""
from __future__ import annotations

import re
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

    def candidates(self, serial: str) -> list[bytes]:
        s = self._store.get(serial)
        return [s] if s is not None else []

    def as_dict(self) -> dict[str, bytes]:
        """Snapshot -- for tests/inspection only, not part of the
        SecretStore protocol."""
        return dict(self._store)


# ======================================================================= #
# Keys file
# ======================================================================= #
_FALLBACK_TOKENS = {
    "*", "any", "all", "default", "rest", "others",
    "all others", "all-others", "other", "any-other", "anyother",
}


class KeysFileError(ValueError):
    """A keys file line didn't parse."""


def parse_keys_file(text: str) -> tuple[dict[str, bytes], list[bytes]]:
    """Parse a keys file into ``(by_serial, fallbacks)``.

    One entry per line, ``<tag> <sep> <64 hex chars>``, where ``<sep>`` is
    whitespace or any of ``/ : = ,``. ``<tag>`` is a device serial
    (``DriverPoE-A4CF12B93D08``), the bare 12-hex MAC tail
    (``A4CF12B93D08``), or a fallback token (``*``, ``any``,
    ``all others``, ...). ``#`` starts a comment; blank lines are ignored.

    More than one fallback line is allowed -- each fallback key is tried,
    in file order, for a serial that has no explicit entry. Handy when
    some units were re-keyed and the rest are still on the factory
    default and you don't want to track which is which.
    """
    by_serial: dict[str, bytes] = {}
    fallbacks: list[bytes] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.search(r"([0-9a-fA-F]{64})\s*$", line)
        if not m:
            raise KeysFileError(f"keys file line {lineno}: no 64-hex-character key found")
        key = bytes.fromhex(m.group(1))
        tag = line[: m.start()].strip().strip("/:=,").strip()
        if not tag or tag.lower() in _FALLBACK_TOKENS:
            if key not in fallbacks:
                fallbacks.append(key)
        else:
            by_serial[tag] = key
    if not by_serial and not fallbacks:
        raise KeysFileError("keys file is empty")
    return by_serial, fallbacks


class KeyfileSecretStore:
    """serial -> key(s), from a keys file the operator loads each session.
    RAM only -- ``set()``/``delete()`` update the in-memory map so the
    session keeps working after a CHANGE_SECRET, but nothing is ever
    written anywhere.

    ``candidates()`` yields, in order: the exact-serial key, the bare
    MAC-tail key, then every fallback key. ``get()`` is just the first of
    those (for callers that want a single value); find_working_secret()
    uses ``candidates()`` and tries them all.
    """

    def __init__(self, keys: dict[str, bytes] | None = None, fallbacks: list[bytes] | None = None) -> None:
        self._keys: dict[str, bytes] = dict(keys or {})
        self._fallbacks: list[bytes] = list(fallbacks or [])

    def load_text(self, text: str) -> None:
        self._keys, self._fallbacks = parse_keys_file(text)

    def clear(self) -> None:
        self._keys = {}
        self._fallbacks = []

    @property
    def loaded(self) -> bool:
        return bool(self._keys or self._fallbacks)

    def stats(self) -> dict[str, object]:
        return {"count": len(self._keys), "fallback_count": len(self._fallbacks)}

    def candidates(self, serial: str) -> list[bytes]:
        out: list[bytes] = []
        if serial in self._keys:
            out.append(self._keys[serial])
        tail = serial.rpartition("-")[2]
        if tail and tail in self._keys and self._keys[tail] not in out:
            out.append(self._keys[tail])
        for fb in self._fallbacks:
            if fb not in out:
                out.append(fb)
        return out

    def get(self, serial: str) -> bytes | None:
        cands = self.candidates(serial)
        return cands[0] if cands else None

    def set(self, serial: str, secret: bytes) -> None:
        if len(secret) != SECRET_LEN:
            raise ValueError(f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
        self._keys[serial] = secret

    def delete(self, serial: str) -> None:
        self._keys.pop(serial, None)
