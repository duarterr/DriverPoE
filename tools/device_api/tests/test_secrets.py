"""Tests for device_api.secrets -- no network involved."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from device_api.secrets import (
    ADMIN_DEFAULT_SECRET,
    SECRET_LEN,
    EncryptedFileSecretStore,
    MemorySecretStore,
    VaultFormatError,
    VaultLocked,
    VaultMissing,
    bootstrap_store,
    default_vault_path,
)

S32 = bytes(range(32))


class TestAdminDefaultSecret(unittest.TestCase):
    def test_length(self):
        self.assertEqual(len(ADMIN_DEFAULT_SECRET), SECRET_LEN)

    def test_matches_documented_ascii_value(self):
        # MUST match ADMIN_DEFAULT_SECRET in main/poe_luminaire_main.h byte for byte.
        self.assertEqual(ADMIN_DEFAULT_SECRET, b"DriverPoE-default-admin-secret!!")


class TestEncryptedFileSecretStore(unittest.TestCase):
    def setUp(self):
        # shrink scrypt for the test run only
        import device_api.secrets as secmod
        self._n = secmod.VAULT_SCRYPT_N
        secmod.VAULT_SCRYPT_N = 1 << 12
        self._dir = tempfile.TemporaryDirectory()
        self.path = Path(self._dir.name) / "s.vault"

    def tearDown(self):
        import device_api.secrets as secmod
        secmod.VAULT_SCRYPT_N = self._n
        self._dir.cleanup()

    def _new(self, pw="pw"):
        return EncryptedFileSecretStore(self.path, pw, create=True)

    def test_create_then_reopen_roundtrip(self):
        s = self._new()
        self.assertTrue(s.is_new)
        s.set("DriverPoE-A4CF12B93D08", S32)
        self.assertFalse(s.is_new)
        reopened = EncryptedFileSecretStore(self.path, "pw")
        self.assertEqual(reopened.get("DriverPoE-A4CF12B93D08"), S32)
        self.assertEqual(reopened.serials(), ["DriverPoE-A4CF12B93D08"])

    def test_wrong_passphrase_rejected(self):
        self._new().set("S1", S32)
        with self.assertRaises(VaultLocked):
            EncryptedFileSecretStore(self.path, "not-the-passphrase")

    def test_missing_vault_without_create_raises(self):
        with self.assertRaises(VaultMissing):
            EncryptedFileSecretStore(self.path, "pw")

    def test_new_vault_writes_nothing_until_first_set(self):
        self._new()
        self.assertFalse(self.path.exists())

    def test_delete_and_delete_missing(self):
        s = self._new()
        s.set("S1", S32)
        s.delete("S1")
        s.delete("never-existed")  # no-op, must not raise
        self.assertIsNone(EncryptedFileSecretStore(self.path, "pw").get("S1"))

    def test_wrong_length_rejected(self):
        with self.assertRaises(ValueError):
            self._new().set("S1", b"too short")

    def test_independent_serials(self):
        s = self._new()
        s.set("S1", bytes([1] * 32))
        s.set("S2", bytes([2] * 32))
        r = EncryptedFileSecretStore(self.path, "pw")
        self.assertEqual(r.get("S1"), bytes([1] * 32))
        self.assertEqual(r.get("S2"), bytes([2] * 32))

    def test_garbage_file_is_format_error(self):
        self.path.write_text("not a vault{{{")
        with self.assertRaises(VaultFormatError):
            EncryptedFileSecretStore(self.path, "pw")

    def test_empty_passphrase_rejected(self):
        from device_api.secrets import VaultError
        with self.assertRaises(VaultError):
            EncryptedFileSecretStore(self.path, "", create=True)

    def test_ciphertext_has_no_plaintext_secret(self):
        s = self._new()
        s.set("S1", S32)
        blob = self.path.read_bytes()
        self.assertNotIn(S32, blob)
        self.assertNotIn(S32.hex().encode(), blob)


class TestBootstrapStore(unittest.TestCase):
    def test_no_vault_gives_memory_store(self):
        with tempfile.TemporaryDirectory() as d:
            state, store = bootstrap_store(Path(d) / "absent.vault")
            self.assertEqual(state, "memory")
            self.assertIsInstance(store, MemorySecretStore)

    def test_vault_present_no_passphrase_is_locked(self):
        import device_api.secrets as secmod
        old = secmod.VAULT_SCRYPT_N
        secmod.VAULT_SCRYPT_N = 1 << 12
        try:
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "s.vault"
                EncryptedFileSecretStore(p, "pw", create=True).set("S1", S32)
                self.assertNotIn("DRIVERPOE_VAULT_PASSPHRASE", __import__("os").environ)
                state, store = bootstrap_store(p)
                self.assertEqual(state, "locked")
                self.assertIsNone(store)
        finally:
            secmod.VAULT_SCRYPT_N = old

    def test_default_vault_path_outside_repo(self):
        p = default_vault_path()
        self.assertEqual(p.name, "secrets.vault")
        self.assertIn("driverpoe", str(p).lower())
        repo_root = Path(__file__).resolve().parents[3]
        self.assertFalse(str(p.resolve()).startswith(str(repo_root)))


class TestMemorySecretStore(unittest.TestCase):
    def test_roundtrip(self):
        store = MemorySecretStore()
        store.set("S1", bytes(range(32)))
        self.assertEqual(store.get("S1"), bytes(range(32)))

    def test_missing_returns_none(self):
        store = MemorySecretStore()
        self.assertIsNone(store.get("nope"))

    def test_initial_values(self):
        store = MemorySecretStore({"S1": bytes(range(32))})
        self.assertEqual(store.get("S1"), bytes(range(32)))

    def test_delete(self):
        store = MemorySecretStore({"S1": bytes(range(32))})
        store.delete("S1")
        self.assertIsNone(store.get("S1"))

    def test_wrong_length_rejected(self):
        store = MemorySecretStore()
        with self.assertRaises(ValueError):
            store.set("S1", b"short")


if __name__ == "__main__":
    unittest.main()
