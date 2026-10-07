"""Tests for device_api.discovery."""
from __future__ import annotations

import socket
import unittest

from device_api.discovery import (
    broadcast_address_for,
    broadcast_info,
    list_local_ipv4s,
    resolve_device_by_ip,
)
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

    def test_bind_ip_still_reaches_a_real_device(self):
        # Binding the scan socket to a specific local address (the
        # --iface case) must not break an ordinary broadcast/unicast --
        # loopback stands in for "one specific NIC" here.
        with FakeDevice() as device:
            results = broadcast_info("127.0.0.1", device.port, timeout=1.0, bind_ip="127.0.0.1")
        self.assertEqual([r.serial for r in results], [SERIAL])

    def test_bind_ip_rejects_an_address_this_host_doesnt_have(self):
        with self.assertRaises(OSError):
            broadcast_info("127.0.0.1", port=1, timeout=0.2, bind_ip="203.0.113.1")


class TestBroadcastAddressFor(unittest.TestCase):
    def test_assumes_a_slash_24(self):
        self.assertEqual(broadcast_address_for("192.168.1.42"), "192.168.1.255")
        self.assertEqual(broadcast_address_for("10.0.0.1"), "10.0.0.255")


class TestListLocalIpv4s(unittest.TestCase):
    def test_returns_a_list_without_loopback(self):
        addrs = list_local_ipv4s()
        self.assertIsInstance(addrs, list)
        self.assertTrue(all(isinstance(a, str) and not a.startswith("127.") for a in addrs))


if __name__ == "__main__":
    unittest.main()
