"""Tests for device_api.discovery."""
from __future__ import annotations

import socket
import unittest

from device_api.discovery import broadcast_info, resolve_device_by_ip
from device_api.tests.test_client import SERIAL, FakeDevice


class TestResolveDeviceByIp(unittest.TestCase):
    def test_resolves_a_live_device(self):
        with FakeDevice() as device:
            info = resolve_device_by_ip("127.0.0.1", device.port, timeout=1.0)
        self.assertIsNotNone(info)
        self.assertEqual(info.serial, SERIAL)

    def test_returns_none_when_nobody_answers(self):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        unused_port = probe.getsockname()[1]
        probe.close()
        info = resolve_device_by_ip("127.0.0.1", unused_port, timeout=0.3)
        self.assertIsNone(info)


class TestBroadcastInfo(unittest.TestCase):
    def test_empty_when_nothing_answers(self):
        # 127.0.0.1 isn't a broadcast address, so this just exercises the
        # "nobody answered within the timeout" path without needing a real
        # LAN broadcast domain in the test environment.
        results = broadcast_info("127.0.0.1", port=1, timeout=0.2)
        self.assertEqual(results, [])


if __name__ == "__main__":
    unittest.main()
