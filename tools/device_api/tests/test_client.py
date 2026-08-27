"""Tests for device_api.client -- exercised against a minimal in-process
fake "device" (a real UDP socket on 127.0.0.1, not a mock) so the tests
cover the actual wire round trip, not just mocked-out method calls.
"""
from __future__ import annotations

import hashlib
import socket
import struct
import threading
import unittest

from device_api.client import (
    AdminClient,
    AuthError,
    DeviceTimeoutError,
    connect,
    find_working_secret,
)
from device_api.models import CommandResult
from device_api.protocol import (
    HEADER_SIZE,
    AdminStatus,
    Packet,
    PacketType,
    ProtocolError,
    ProtocolVersionMismatchError,
)
from device_api.secrets import ADMIN_DEFAULT_SECRET, SECRET_LEN, MemorySecretStore

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    AESGCM = None

SERIAL = "DriverPoE-A4CF12B93D08"
SECRET = bytes(range(32))


def _info_payload() -> bytes:
    buf = bytearray()
    buf += bytes.fromhex("A4CF12B93D08")           # mac
    buf += b"1.0.0".ljust(16, b"\x00")               # fw_version
    buf += bytes((10, 0, 0, 5))                       # ip
    buf += struct.pack(">I", 100)                     # uptime_s
    buf += bytes([1, 1, 1, 1, 1, 1, 1, 1, 50, 0])      # reset_reason..ramp_pending
    buf += struct.pack(">I", 48000)                    # vbus_mv
    buf += struct.pack(">i", 3300)                      # led_voltage_mv
    # DMX status block: disabled, no source, level 0, 0 fps, universe 0/1, no ip
    buf += bytes([0, 0, 0, 0]) + struct.pack(">H", 0) + struct.pack(">H", 1) + bytes(4)
    return bytes(buf)


class FakeDevice:
    """Just enough of the firmware's admin_channel.c to test the client
    against: answers INFO unauthenticated, CHALLENGE with a real HMAC
    check and a fresh nonce, and one configurable write command response.
    Runs its receive loop in a background thread; not a general-purpose
    protocol implementation, only what these tests need."""

    def __init__(self, secret: bytes = SECRET, serial: str = SERIAL):
        self.secret = secret
        self.serial = serial
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self._last_nonce = b""
        self.next_status = AdminStatus.OK
        # OTA test state -- see OTA_BEGIN/OTA_CHUNK/OTA_END/OTA_ABORT below.
        self.ota_in_progress = False
        self.ota_total_size = 0
        self.ota_expected_hash = b""
        self.ota_data = bytearray()
        self.ota_begin_status = AdminStatus.OK  # override to simulate e.g. ERR_BUSY
        self.drop_chunk_offsets: set[int] = set()  # each offset in here is dropped ONCE (no response), to test client-side retry
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self.sock.close()

    def __enter__(self) -> "FakeDevice":
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def _send(self, addr, ptype, nonce, payload, key) -> None:
        pkt = Packet(type=ptype, serial=self.serial, nonce=nonce, payload=payload)
        self.sock.sendto(pkt.pack(key), addr)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                pkt = Packet.unpack(data)
            except ProtocolError:
                continue

            if pkt.type == PacketType.INFO:
                self._send(addr, PacketType.INFO_RESP, b"\x00" * 16, _info_payload(), None)
                continue

            if not pkt.verify_hmac(self.secret):
                continue  # silent drop, like the real firmware

            if pkt.type == PacketType.CHALLENGE:
                self._last_nonce = hashlib.sha256(data).digest()[:16]  # deterministic "random" nonce
                self._send(addr, PacketType.CHALLENGE_RESP, self._last_nonce, b"", self.secret)
                continue

            if pkt.type == PacketType.CHANGE_SECRET:
                if AESGCM is not None:
                    try:
                        new_secret = AESGCM(self.secret).decrypt(pkt.nonce, pkt.payload, pkt._covered[:HEADER_SIZE])
                    except Exception:
                        self._send(addr, PacketType.CHANGE_SECRET_RESP, pkt.nonce,
                                    bytes([int(AdminStatus.ERR_BAD_ARG)]), self.secret)
                        continue
                    self.secret = new_secret
                    self._send(addr, PacketType.CHANGE_SECRET_RESP, pkt.nonce,
                                bytes([int(self.next_status)]), self.secret)
                continue

            if pkt.type == PacketType.OTA_BEGIN:
                if len(pkt.payload) != 36:
                    self._send(addr, PacketType.OTA_BEGIN_RESP, pkt.nonce,
                                bytes([int(AdminStatus.ERR_BAD_ARG)]), self.secret)
                    continue
                self.ota_total_size = int.from_bytes(pkt.payload[0:4], "big")
                self.ota_expected_hash = pkt.payload[4:36]
                self.ota_data = bytearray()
                self.ota_in_progress = (self.ota_begin_status == AdminStatus.OK)
                self._send(addr, PacketType.OTA_BEGIN_RESP, pkt.nonce, bytes([int(self.ota_begin_status)]), self.secret)
                continue

            if pkt.type == PacketType.OTA_CHUNK:
                offset = int.from_bytes(pkt.payload[0:4], "big")
                data = pkt.payload[4:]
                if offset in self.drop_chunk_offsets:
                    self.drop_chunk_offsets.discard(offset)  # simulate ONE lost packet -- no response at all
                    continue
                if not self.ota_in_progress:
                    resp = bytes([int(AdminStatus.ERR_BAD_ARG)]) + (0).to_bytes(4, "big")
                elif offset < len(self.ota_data):
                    resp = bytes([int(AdminStatus.OK)]) + len(self.ota_data).to_bytes(4, "big")  # duplicate -- re-ack
                elif offset != len(self.ota_data):
                    resp = bytes([int(AdminStatus.ERR_BAD_ARG)]) + len(self.ota_data).to_bytes(4, "big")  # gap
                else:
                    self.ota_data += data
                    resp = bytes([int(AdminStatus.OK)]) + len(self.ota_data).to_bytes(4, "big")
                self._send(addr, PacketType.OTA_CHUNK_RESP, pkt.nonce, resp, self.secret)
                continue

            if pkt.type == PacketType.OTA_END:
                if not self.ota_in_progress or len(self.ota_data) != self.ota_total_size:
                    status = AdminStatus.ERR_BAD_ARG
                elif hashlib.sha256(bytes(self.ota_data)).digest() != self.ota_expected_hash:
                    status = AdminStatus.ERR_BAD_ARG
                else:
                    status = AdminStatus.OK
                self.ota_in_progress = False
                self._send(addr, PacketType.OTA_END_RESP, pkt.nonce, bytes([int(status)]), self.secret)
                continue

            if pkt.type == PacketType.OTA_ABORT:
                self.ota_in_progress = False
                self.ota_data = bytearray()
                self._send(addr, PacketType.OTA_ABORT_RESP, pkt.nonce, bytes([int(AdminStatus.OK)]), self.secret)
                continue

            resp_type = PacketType(int(pkt.type) | 0x80)
            status = bytes([int(self.next_status)])
            self._send(addr, resp_type, pkt.nonce, status, self.secret)


class TestAdminClientInfo(unittest.TestCase):
    def test_info_roundtrip(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                info = client.info()
        self.assertEqual(info.serial, SERIAL)
        self.assertEqual(info.mac_str, "A4:CF:12:B9:3D:08")
        self.assertEqual(info.fw_version, "1.0.0")
        self.assertEqual(info.vbus_mv, 48000)

    def test_timeout_when_nobody_answers(self):
        # An unused local UDP port that nothing is bound to -- recvfrom()
        # will time out, never receive a reply.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        unused_port = probe.getsockname()[1]
        probe.close()
        with AdminClient("127.0.0.1", unused_port, timeout=0.3) as client:
            with self.assertRaises(DeviceTimeoutError):
                client.info()

    def test_version_mismatch_surfaces_as_specific_error(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.5)
        port = sock.getsockname()[1]

        def responder():
            data, addr = sock.recvfrom(4096)
            bad = bytearray(data)
            bad[4] = 200  # corrupt the version byte of whatever request arrives
            sock.sendto(bytes(bad), addr)

        t = threading.Thread(target=responder, daemon=True)
        t.start()
        try:
            with AdminClient("127.0.0.1", port, timeout=1.0) as client:
                with self.assertRaises(ProtocolVersionMismatchError):
                    client.info()
        finally:
            t.join(timeout=1)
            sock.close()


class TestAdminClientChallengeAndWrites(unittest.TestCase):
    def test_challenge_returns_a_16_byte_nonce(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                nonce = client.challenge(SECRET, SERIAL)
                self.assertEqual(len(nonce), 16)

    def test_challenge_with_wrong_secret_times_out_not_autherror(self):
        """The firmware silently drops any packet whose HMAC doesn't
        verify (anti-oracle design, see admin_channel.c's file-top
        comment) -- a wrong secret against a real device NEVER gets an
        authenticated rejection back, it just times out. AuthError is
        reserved for the (currently unreachable in practice, but still
        handled) case of an actual authenticated-but-wrong-type response.
        This is exactly why find_working_secret() treats the two the same
        when trying candidates -- see its own comment."""
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=0.3) as client:
                with self.assertRaises(DeviceTimeoutError):
                    client.challenge(b"\x00" * 32, SERIAL)

    def test_on_applies_immediately_when_status_ok(self):
        with FakeDevice() as device:
            device.next_status = AdminStatus.OK
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.on(SECRET, SERIAL, ramp_ms=250)
        self.assertIsInstance(result, CommandResult)
        self.assertTrue(result.applied)
        self.assertTrue(result.accepted)
        self.assertFalse(result.pending)

    def test_on_pending_when_power_not_ready(self):
        with FakeDevice() as device:
            device.next_status = AdminStatus.ACCEPTED_PENDING
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.on(SECRET, SERIAL)
        self.assertTrue(result.accepted)
        self.assertFalse(result.applied)
        self.assertTrue(result.pending)

    def test_refused_command_does_not_raise_by_default(self):
        with FakeDevice() as device:
            device.next_status = AdminStatus.ERR_BAD_ARG
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.off(SECRET, SERIAL)
        self.assertFalse(result.accepted)
        self.assertFalse(result.applied)

    def test_raise_if_refused_raises_command_refused_error(self):
        from device_api.client import CommandRefusedError
        with FakeDevice() as device:
            device.next_status = AdminStatus.ERR_NOT_READY
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.identify(SECRET, SERIAL)
                with self.assertRaises(CommandRefusedError):
                    result.raise_if_refused()

    def test_dim_rejects_out_of_range_percent(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                with self.assertRaises(ValueError):
                    client.dim(SECRET, SERIAL, 150)


class TestFindWorkingSecret(unittest.TestCase):
    def test_finds_stored_secret_first(self):
        with FakeDevice(secret=SECRET) as device:
            store = MemorySecretStore({SERIAL: SECRET})
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                secret, nonce = find_working_secret(client, SERIAL, store)
        self.assertEqual(secret, SECRET)
        self.assertEqual(len(nonce), 16)

    def test_falls_back_to_factory_default(self):
        with FakeDevice(secret=ADMIN_DEFAULT_SECRET) as device:
            store = MemorySecretStore()  # nothing saved
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                secret, _nonce = find_working_secret(client, SERIAL, store)
        self.assertEqual(secret, ADMIN_DEFAULT_SECRET)

    def test_prompts_manually_as_last_resort(self):
        custom = bytes([7] * 32)
        with FakeDevice(secret=custom) as device:
            store = MemorySecretStore()
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                secret, _nonce = find_working_secret(client, SERIAL, store, manual_secret_provider=lambda: custom)
        self.assertEqual(secret, custom)

    def test_raises_without_a_manual_provider(self):
        with FakeDevice(secret=bytes([9] * 32)) as device:
            store = MemorySecretStore()
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                with self.assertRaises(AuthError):
                    find_working_secret(client, SERIAL, store)

    def test_wrong_manual_secret_raises_autherror_not_timeout(self):
        """The device silently drops a CHALLENGE with a wrong secret
        (never sends an authenticated rejection) -- a manually-typed wrong
        secret must still surface as AuthError, not DeviceTimeoutError, so
        every caller only needs to catch one exception type here."""
        with FakeDevice(secret=bytes([9] * 32)) as device:
            store = MemorySecretStore()
            with AdminClient("127.0.0.1", device.port, timeout=0.3) as client:
                with self.assertRaises(AuthError):
                    find_working_secret(client, SERIAL, store, manual_secret_provider=lambda: bytes([1] * 32))


@unittest.skipIf(AESGCM is None, "cryptography package not installed")
class TestChangeSecret(unittest.TestCase):
    def test_change_secret_roundtrip_and_reauth(self):
        with FakeDevice() as device:
            new_secret = bytes([42] * SECRET_LEN)
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result, applied = client.change_secret(SECRET, SERIAL, new_secret)
                self.assertTrue(result.applied)
                self.assertEqual(applied, new_secret)
                # The old secret must no longer authenticate anything...
                with self.assertRaises(DeviceTimeoutError):
                    client.challenge(SECRET, SERIAL)
                # ...but the new one does.
                nonce = client.challenge(new_secret, SERIAL)
                self.assertEqual(len(nonce), 16)

    def test_change_secret_generates_random_secret_when_none_given(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result, generated = client.change_secret(SECRET, SERIAL, None)
                self.assertTrue(result.applied)
                self.assertEqual(len(generated), SECRET_LEN)

    def test_change_secret_rejects_wrong_length(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                with self.assertRaises(ValueError):
                    client.change_secret(SECRET, SERIAL, b"too short")


class TestOtaUpdate(unittest.TestCase):
    def test_happy_path_full_transfer_and_progress_callback(self):
        image = bytes((i * 37) % 256 for i in range(3000))  # multi-chunk (OTA_CHUNK_MAX_DATA=1024)
        progress_calls = []
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.ota_update(SECRET, SERIAL, image, progress_callback=lambda sent, total: progress_calls.append((sent, total)))
        self.assertTrue(result.applied)
        self.assertEqual(bytes(device.ota_data), image)
        self.assertEqual(progress_calls, [(1024, 3000), (2048, 3000), (3000, 3000)])

    def test_small_single_chunk_image(self):
        image = b"\x01\x02\x03\x04\x05"
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                result = client.ota_update(SECRET, SERIAL, image)
        self.assertTrue(result.applied)
        self.assertEqual(bytes(device.ota_data), image)

    def test_empty_image_rejected_locally(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                with self.assertRaises(ValueError):
                    client.ota_update(SECRET, SERIAL, b"")

    def test_busy_raises_ota_transfer_error(self):
        from device_api.client import OtaTransferError
        with FakeDevice() as device:
            device.ota_begin_status = AdminStatus.ERR_BUSY
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                with self.assertRaises(OtaTransferError):
                    client.ota_update(SECRET, SERIAL, b"\x01\x02\x03")

    def test_hash_mismatch_reported_as_unapplied_not_raised(self):
        """A refusal at OTA_END (the transfer itself succeeded, only the
        final hash/size check failed) comes back as an ordinary
        CommandResult, not an exception -- see ota_update()'s docstring."""
        image = b"\x01\x02\x03\x04\x05"
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                # Force the hash ota_update() sends in OTA_BEGIN to be
                # wrong (without touching the actual transferred bytes),
                # by making the FIRST hashlib.sha256() call (the one
                # computing the announced image hash) return a digest of
                # tampered data, then restoring normal behavior for
                # everything else (the FakeDevice's own OTA_END check).
                orig_sha256 = hashlib.sha256
                calls = {"n": 0}

                def tampering_sha256(data=b""):
                    calls["n"] += 1
                    if calls["n"] == 1:
                        return orig_sha256(data + b"\x00")  # wrong hash sent in OTA_BEGIN
                    return orig_sha256(data)

                hashlib.sha256 = tampering_sha256
                try:
                    result = client.ota_update(SECRET, SERIAL, image)
                finally:
                    hashlib.sha256 = orig_sha256
        self.assertFalse(result.applied)
        self.assertEqual(result.status, AdminStatus.ERR_BAD_ARG)

    def test_dropped_chunk_response_is_retried_and_recovers(self):
        image = bytes(range(200))
        with FakeDevice() as device:
            device.drop_chunk_offsets = {0}  # the first chunk's response is lost once
            with AdminClient("127.0.0.1", device.port, timeout=0.3) as client:
                result = client.ota_update(SECRET, SERIAL, image, max_retries_per_chunk=3)
        self.assertTrue(result.applied)
        self.assertEqual(bytes(device.ota_data), image)

    def test_chunk_exhausts_retries_raises_timeout(self):
        image = bytes(range(200))
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=0.2) as client:
                # Patch the chunk handling to always drop offset 0 by
                # re-adding it after each discard (simulates a link that
                # always loses this specific chunk).
                class AlwaysDrop(set):
                    def discard(self, item):
                        pass  # never actually removes it -- every attempt is dropped
                device.drop_chunk_offsets = AlwaysDrop({0})
                with self.assertRaises(DeviceTimeoutError):
                    client.ota_update(SECRET, SERIAL, image, max_retries_per_chunk=2)

    def test_ota_abort(self):
        with FakeDevice() as device:
            with AdminClient("127.0.0.1", device.port, timeout=1.0) as client:
                begin_nonce = client.challenge(SECRET, SERIAL)
                client._write_command("OTA_BEGIN", PacketType.OTA_BEGIN, PacketType.OTA_BEGIN_RESP,
                                       SERIAL, SECRET, begin_nonce, (10).to_bytes(4, "big") + b"\x00" * 32)
                result = client.ota_abort(SECRET, SERIAL)
        self.assertTrue(result.applied)
        self.assertFalse(device.ota_in_progress)


class TestConnect(unittest.TestCase):
    def test_connect_returns_client_info_secret(self):
        with FakeDevice() as device:
            store = MemorySecretStore({SERIAL: SECRET})
            client, info, secret = connect("127.0.0.1", store, port=device.port, timeout=1.0)
            try:
                self.assertEqual(info.serial, SERIAL)
                self.assertEqual(secret, SECRET)
            finally:
                client.close()


if __name__ == "__main__":
    unittest.main()
