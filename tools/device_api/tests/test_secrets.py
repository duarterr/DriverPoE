"""Tests for device_api.secrets -- no network involved."""
from __future__ import annotations

import unittest

from device_api.secrets import (
    ADMIN_DEFAULT_SECRET,
    SECRET_LEN,
    KeyfileSecretStore,
    KeysFileError,
    MemorySecretStore,
    parse_keys_file,
)

S32 = bytes(range(32))
K1 = ("11" * 32)
K2 = ("22" * 32)
KF = ("ff" * 32)


class TestAdminDefaultSecret(unittest.TestCase):
    def test_length(self):
        self.assertEqual(len(ADMIN_DEFAULT_SECRET), SECRET_LEN)

    def test_matches_documented_ascii_value(self):
        # MUST match ADMIN_DEFAULT_SECRET in main/poe_luminaire_main.h byte for byte.
        self.assertEqual(ADMIN_DEFAULT_SECRET, b"DriverPoE-default-admin-secret!!")


class TestParseKeysFile(unittest.TestCase):
    def test_by_serial_and_mac_and_fallback(self):
        text = f"""
            # my rig
            DriverPoE-A4CF12B93D08  {K1}
            0011223344FF          / {K2}
            all others            = {KF}
        """
        by_serial, fallbacks = parse_keys_file(text)
        self.assertEqual(by_serial["DriverPoE-A4CF12B93D08"], bytes.fromhex(K1))
        self.assertEqual(by_serial["0011223344FF"], bytes.fromhex(K2))
        self.assertEqual(fallbacks, [bytes.fromhex(KF)])

    def test_separators(self):
        for sep in ("  ", " / ", ":", " = ", ", "):
            by_serial, _ = parse_keys_file(f"S1{sep}{K1}")
            self.assertEqual(by_serial["S1"], bytes.fromhex(K1))

    def test_fallback_tokens(self):
        for tok in ("*", "any", "all others", "ALL OTHERS", "default", "rest"):
            _, fallbacks = parse_keys_file(f"{tok} {KF}")
            self.assertEqual(fallbacks, [bytes.fromhex(KF)])

    def test_multiple_fallbacks_kept_in_order(self):
        _, fallbacks = parse_keys_file(f"all others {K1}\n* {K2}\nany {K1}")
        self.assertEqual(fallbacks, [bytes.fromhex(K1), bytes.fromhex(K2)])  # dedup, order preserved

    def test_bare_key_line_is_fallback(self):
        _, fallbacks = parse_keys_file(KF)
        self.assertEqual(fallbacks, [bytes.fromhex(KF)])

    def test_comments_and_blank_lines_ignored(self):
        by_serial, fbs = parse_keys_file(f"# header\n\n   \nS1 {K1}   # inline comment\n\n")
        self.assertEqual(by_serial, {"S1": bytes.fromhex(K1)})
        self.assertEqual(fbs, [])

    def test_empty_file_rejected(self):
        with self.assertRaises(KeysFileError):
            parse_keys_file("# just a comment\n\n")

    def test_bad_line_rejected(self):
        with self.assertRaises(KeysFileError):
            parse_keys_file("S1 not-a-64-hex-key")


class TestKeyfileSecretStore(unittest.TestCase):
    def _store(self):
        s = KeyfileSecretStore()
        s.load_text(f"DriverPoE-AAAAAAAAAAAA {K1}\nBBBBBBBBBBBB {K2}\n* {KF}")
        return s

    def test_get_exact_serial(self):
        self.assertEqual(self._store().get("DriverPoE-AAAAAAAAAAAA"), bytes.fromhex(K1))

    def test_get_by_mac_tail(self):
        self.assertEqual(self._store().get("DriverPoE-BBBBBBBBBBBB"), bytes.fromhex(K2))

    def test_get_falls_back(self):
        self.assertEqual(self._store().get("DriverPoE-ZZZZZZZZZZZZ"), bytes.fromhex(KF))

    def test_no_fallback_returns_none(self):
        s = KeyfileSecretStore()
        s.load_text(f"S1 {K1}")
        self.assertIsNone(s.get("S2"))
        self.assertEqual(s.candidates("S2"), [])

    def test_candidates_order(self):
        s = KeyfileSecretStore()
        s.load_text(f"DriverPoE-CCCCCCCCCCCC {K1}\nall others {K2}\n* {KF}")
        self.assertEqual(s.candidates("DriverPoE-CCCCCCCCCCCC"),
                         [bytes.fromhex(K1), bytes.fromhex(K2), bytes.fromhex(KF)])
        self.assertEqual(s.candidates("DriverPoE-ZZZZZZZZZZZZ"),
                         [bytes.fromhex(K2), bytes.fromhex(KF)])

    def test_set_is_session_only_and_get_reflects_it(self):
        s = self._store()
        s.set("S-new", S32)
        self.assertEqual(s.get("S-new"), S32)

    def test_set_wrong_length_rejected(self):
        with self.assertRaises(ValueError):
            self._store().set("S1", b"short")

    def test_delete_and_delete_missing(self):
        s = self._store()
        s.delete("DriverPoE-AAAAAAAAAAAA")
        s.delete("never")  # no-op
        # falls back now
        self.assertEqual(s.get("DriverPoE-AAAAAAAAAAAA"), bytes.fromhex(KF))

    def test_clear_and_loaded_flag(self):
        s = self._store()
        self.assertTrue(s.loaded)
        s.clear()
        self.assertFalse(s.loaded)
        self.assertIsNone(s.get("anything"))

    def test_stats(self):
        self.assertEqual(self._store().stats(), {"count": 2, "fallback_count": 1})


class TestMemorySecretStore(unittest.TestCase):
    def test_roundtrip(self):
        store = MemorySecretStore()
        store.set("S1", bytes(range(32)))
        self.assertEqual(store.get("S1"), bytes(range(32)))

    def test_missing_returns_none(self):
        self.assertIsNone(MemorySecretStore().get("nope"))

    def test_initial_values(self):
        self.assertEqual(MemorySecretStore({"S1": S32}).get("S1"), S32)

    def test_delete(self):
        store = MemorySecretStore({"S1": S32})
        store.delete("S1")
        self.assertIsNone(store.get("S1"))

    def test_wrong_length_rejected(self):
        with self.assertRaises(ValueError):
            MemorySecretStore().set("S1", b"short")


if __name__ == "__main__":
    unittest.main()
