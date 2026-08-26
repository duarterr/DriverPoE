"""Tests for driverpoe.secrets -- no network involved."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from driverpoe.secrets import ADMIN_DEFAULT_SECRET, SECRET_LEN, JsonFileSecretStore, MemorySecretStore


class TestAdminDefaultSecret(unittest.TestCase):
    def test_length(self):
        self.assertEqual(len(ADMIN_DEFAULT_SECRET), SECRET_LEN)

    def test_matches_documented_ascii_value(self):
        # MUST match ADMIN_DEFAULT_SECRET in main/poe_luminaire_main.h byte for byte.
        self.assertEqual(ADMIN_DEFAULT_SECRET, b"DriverPoE-default-admin-secret!!")


class TestJsonFileSecretStore(unittest.TestCase):
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "admin_secrets.json"
            store = JsonFileSecretStore(path)
            secret = bytes(range(32))
            store.set("DriverPoE-A4CF12B93D08", secret)
            self.assertEqual(store.get("DriverPoE-A4CF12B93D08"), secret)

    def test_missing_file_returns_none(self):
        store = JsonFileSecretStore(Path("/nonexistent/admin_secrets.json"))
        self.assertIsNone(store.get("anything"))

    def test_corrupt_file_treated_as_empty(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "admin_secrets.json"
            path.write_text("not valid json{{{")
            store = JsonFileSecretStore(path)
            self.assertIsNone(store.get("DriverPoE-A4CF12B93D08"))

    def test_delete(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "admin_secrets.json"
            store = JsonFileSecretStore(path)
            store.set("S1", bytes(range(32)))
            store.delete("S1")
            self.assertIsNone(store.get("S1"))

    def test_delete_missing_serial_is_a_noop(self):
        with tempfile.TemporaryDirectory() as d:
            store = JsonFileSecretStore(Path(d) / "admin_secrets.json")
            store.delete("never-existed")  # must not raise

    def test_wrong_length_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            store = JsonFileSecretStore(Path(d) / "admin_secrets.json")
            with self.assertRaises(ValueError):
                store.set("S1", b"too short")

    def test_independent_serials(self):
        with tempfile.TemporaryDirectory() as d:
            store = JsonFileSecretStore(Path(d) / "admin_secrets.json")
            store.set("S1", bytes([1] * 32))
            store.set("S2", bytes([2] * 32))
            self.assertEqual(store.get("S1"), bytes([1] * 32))
            self.assertEqual(store.get("S2"), bytes([2] * 32))


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
