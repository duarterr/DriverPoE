"""Admin secret storage -- the factory default, plus a small, swappable
storage interface for per-unit secrets an operator has changed away from
it.

Implementations that ship here:

- ``MemorySecretStore`` -- RAM only, nothing on disk. The default when
  there's no vault (secrets are then supplied per session, e.g. via
  client.py's ``find_working_secret(manual_secret_provider=...)``).
- ``EncryptedFileSecretStore`` -- an at-rest-encrypted file (AES-256-GCM,
  key derived from an operator passphrase with scrypt). The passphrase is
  never stored; a wrong one just fails to decrypt. This is the file to
  point the CLI / web UIs at when you don't want to retype every unit's
  secret.

Anything else (OS keychain, HashiCorp Vault, a cloud secrets manager)
implements the same three-method ``SecretStore`` protocol.
"""
from __future__ import annotations

import base64
import json
import os
import stat
import tempfile
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


# ======================================================================= #
# Encrypted vault
# ======================================================================= #
class VaultError(Exception):
    """Base for every deliberate failure of EncryptedFileSecretStore."""


class VaultLocked(VaultError):
    """The passphrase didn't decrypt the vault (wrong passphrase, or the
    file was tampered with / truncated)."""


class VaultFormatError(VaultError):
    """The vault file exists but isn't a vault this version understands."""


class VaultMissing(VaultError):
    """No vault file at the given path and create=False."""


# scrypt work factor. n=2**16 (~64 MiB, well under a second) is plenty for
# a local file unlocked interactively; bump VAULT_SCRYPT_N to re-key
# harder (old files still open -- the params are stored per file).
VAULT_SCRYPT_N = 1 << 16
VAULT_SCRYPT_R = 8
VAULT_SCRYPT_P = 1
_VAULT_VERSION = 1
_VAULT_AAD = b"driverpoe-secret-vault-v1"


def default_vault_path() -> Path:
    """Where the vault lives if the caller doesn't say. Outside the repo
    tree on purpose. Override with $DRIVERPOE_VAULT."""
    env = os.environ.get("DRIVERPOE_VAULT")
    if env:
        return Path(env).expanduser()
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or (Path.home() / "AppData" / "Roaming"))
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or (Path.home() / ".config"))
    return base / "driverpoe" / "secrets.vault"


def _aesgcm(key: bytes):
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError as e:  # pragma: no cover - environment-specific
        raise VaultError(
            "The 'cryptography' package is required for the encrypted vault "
            "(pip install cryptography)."
        ) from e
    return AESGCM(key)


def _derive_key(passphrase: bytes, salt: bytes, n: int, r: int, p: int) -> bytes:
    try:
        from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
    except ImportError as e:  # pragma: no cover - environment-specific
        raise VaultError(
            "The 'cryptography' package is required for the encrypted vault "
            "(pip install cryptography)."
        ) from e
    return Scrypt(salt=salt, length=32, n=n, r=r, p=p).derive(passphrase)


class EncryptedFileSecretStore:
    """serial -> secret, persisted as a single AES-256-GCM blob whose key
    is scrypt(passphrase). The passphrase is held in memory (as a derived
    key) only for this instance's lifetime and is never written anywhere.

    The plaintext is decrypted once on construction and kept in memory;
    set()/delete() re-encrypt the whole blob with a fresh nonce and write
    it atomically. Not safe for concurrent writers across processes (last
    writer wins) -- fine for a CLI or a single local web backend.
    """

    def __init__(self, path: Path | str, passphrase: bytes | str, *, create: bool = False) -> None:
        self.path = Path(path).expanduser()
        pw = passphrase.encode("utf-8") if isinstance(passphrase, str) else passphrase
        if not pw:
            raise VaultError("passphrase must not be empty")

        if self.path.exists():
            self._open(pw)
            self._is_new = False
        elif create:
            self._salt = os.urandom(16)
            self._kdf = (VAULT_SCRYPT_N, VAULT_SCRYPT_R, VAULT_SCRYPT_P)
            self._key = _derive_key(pw, self._salt, *self._kdf)
            self._data: dict[str, bytes] = {}
            self._is_new = True
        else:
            raise VaultMissing(f"no vault at {self.path}")

    # -- lifecycle ----------------------------------------------------------
    @property
    def is_new(self) -> bool:
        """True if the vault didn't exist yet (nothing written to disk
        until the first set())."""
        return self._is_new

    def _open(self, pw: bytes) -> None:
        try:
            doc = json.loads(self.path.read_text())
            if doc.get("v") != _VAULT_VERSION or doc.get("kdf", {}).get("name") != "scrypt":
                raise VaultFormatError(f"{self.path} is not a v{_VAULT_VERSION} scrypt vault")
            kdf = doc["kdf"]
            self._salt = base64.b64decode(kdf["salt"])
            self._kdf = (int(kdf["n"]), int(kdf["r"]), int(kdf["p"]))
            nonce = base64.b64decode(doc["nonce"])
            ct = base64.b64decode(doc["ct"])
        except VaultFormatError:
            raise
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise VaultFormatError(f"{self.path} is unreadable or malformed: {e}") from e

        self._key = _derive_key(pw, self._salt, *self._kdf)
        try:
            plaintext = _aesgcm(self._key).decrypt(nonce, ct, _VAULT_AAD)
        except Exception as e:  # cryptography raises InvalidTag
            raise VaultLocked("wrong passphrase, or the vault file is corrupt") from e

        raw = json.loads(plaintext)
        data: dict[str, bytes] = {}
        for serial, hexval in raw.items():
            try:
                secret = bytes.fromhex(hexval)
            except (ValueError, TypeError):
                continue
            if len(secret) == SECRET_LEN:
                data[serial] = secret
        self._data = data

    def _flush(self) -> None:
        plaintext = json.dumps({s: v.hex() for s, v in sorted(self._data.items())}).encode("utf-8")
        nonce = os.urandom(12)
        ct = _aesgcm(self._key).encrypt(nonce, plaintext, _VAULT_AAD)
        n, r, p = self._kdf
        doc = {
            "v": _VAULT_VERSION,
            "kdf": {"name": "scrypt", "n": n, "r": r, "p": p, "salt": base64.b64encode(self._salt).decode()},
            "nonce": base64.b64encode(nonce).decode(),
            "ct": base64.b64encode(ct).decode(),
        }
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".vault-", suffix=".tmp")
        try:
            with os.fdopen(fd, "w") as f:
                json.dump(doc, f, indent=2)
            if os.name == "posix":
                try:
                    os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
                except OSError:
                    pass
            os.replace(tmp, self.path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise
        self._is_new = False

    # -- SecretStore ------------------------------------------------------
    def get(self, serial: str) -> bytes | None:
        return self._data.get(serial)

    def set(self, serial: str, secret: bytes) -> None:
        if len(secret) != SECRET_LEN:
            raise ValueError(f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
        self._data[serial] = secret
        self._flush()

    def delete(self, serial: str) -> None:
        if serial in self._data:
            del self._data[serial]
            self._flush()

    def serials(self) -> list[str]:
        """Every serial the vault has a secret for -- not part of the
        SecretStore protocol; for a "what's in here" listing."""
        return sorted(self._data)


def bootstrap_store(path: Path | str | None = None) -> tuple[str, SecretStore | None]:
    """Pick a secret store for a backend that can't prompt for a
    passphrase at startup (a web UI). Returns ``(state, store)``:

    - ``("unlocked", EncryptedFileSecretStore)`` -- a vault exists and was
      opened with ``$DRIVERPOE_VAULT_PASSPHRASE``.
    - ``("locked", None)`` -- a vault exists but no passphrase was
      available; the backend should expose an unlock endpoint.
    - ``("memory", MemorySecretStore)`` -- no vault; every unit's secret
      is supplied per session (or is still the factory default).

    Raises VaultError only if an env passphrase was given and it didn't
    open the vault.
    """
    p = Path(path).expanduser() if path else default_vault_path()
    if not p.exists():
        return "memory", MemorySecretStore()
    pw = os.environ.get("DRIVERPOE_VAULT_PASSPHRASE")
    if not pw:
        return "locked", None
    return "unlocked", EncryptedFileSecretStore(p, pw)
