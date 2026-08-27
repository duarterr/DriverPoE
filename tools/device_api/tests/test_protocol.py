"""Tests for device_api.protocol -- pure serialization, no sockets."""
from __future__ import annotations

import hashlib
import hmac
import struct
import unittest

from device_api.protocol import (
    DMX_CFG_WIRE_SIZE,
    HEADER_SIZE,
    HMAC_LEN,
    AdminStatus,
    DmxConfig,
    Packet,
    PacketType,
    ProtocolError,
    ProtocolVersionMismatchError,
    mac_from_serial,
    mac_from_str,
    mac_to_str,
    pack_dmx_config,
    parse_dmx_config,
    parse_info_payload,
    serial_from_mac,
    status_byte,
    status_name,
)


class TestSerial(unittest.TestCase):
    def test_serial_format(self):
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        self.assertEqual(serial_from_mac(mac), "DriverPoE-A4CF12B93D08")

    def test_mac_roundtrip(self):
        for s in ("A4:CF:12:B9:3D:08", "a4-cf-12-b9-3d-08", "A4CF12B93D08"):
            self.assertEqual(mac_from_str(s), bytes.fromhex("A4CF12B93D08"))

    def test_mac_invalid(self):
        with self.assertRaises(ValueError):
            mac_from_str("not-a-mac")

    def test_mac_to_str(self):
        self.assertEqual(mac_to_str(bytes.fromhex("A4CF12B93D08")), "A4:CF:12:B9:3D:08")

    def test_mac_from_serial_roundtrip(self):
        mac = mac_from_str("A4:CF:12:B9:3D:08")
        serial = serial_from_mac(mac)
        self.assertEqual(mac_from_serial(serial), mac)

    def test_mac_from_serial_bad_format(self):
        with self.assertRaises(ValueError):
            mac_from_serial("not-a-valid-serial")


class TestPacket(unittest.TestCase):
    def test_roundtrip_authenticated(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.ON, serial="DriverPoE-A4CF12B93D08", nonce=bytes(range(16)), payload=b"hello")
        wire = pkt.pack(hmac_key=key)
        self.assertEqual(len(wire), HEADER_SIZE + 5 + HMAC_LEN)

        parsed = Packet.unpack(wire)
        self.assertEqual(parsed.type, PacketType.ON)
        self.assertEqual(parsed.serial, "DriverPoE-A4CF12B93D08")
        self.assertEqual(parsed.nonce, bytes(range(16)))
        self.assertEqual(parsed.payload, b"hello")
        self.assertTrue(parsed.verify_hmac(key))

    def test_roundtrip_unauthenticated_info(self):
        pkt = Packet(type=PacketType.INFO, serial="", nonce=b"", payload=b"")
        wire = pkt.pack(hmac_key=None)
        parsed = Packet.unpack(wire)
        self.assertEqual(parsed.type, PacketType.INFO)
        self.assertEqual(parsed.payload, b"")

    def test_tampered_payload_fails_size_check(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.REBOOT, serial="DriverPoE-000000000000", nonce=b"\x01" * 16, payload=b"")
        wire = bytearray(pkt.pack(hmac_key=key))
        wire[HEADER_SIZE - 1] ^= 0xFF  # corrupt the last byte before the payload (payload_len)
        with self.assertRaises(ProtocolError):
            Packet.unpack(bytes(wire))

    def test_wrong_key_fails_verify(self):
        pkt = Packet(type=PacketType.ON, serial="DriverPoE-000000000000", nonce=b"", payload=b"x")
        wire = pkt.pack(hmac_key=bytes(range(32)))
        parsed = Packet.unpack(wire)
        self.assertFalse(parsed.verify_hmac(bytes(range(1, 33))))

    def test_short_packet_rejected(self):
        with self.assertRaises(ProtocolError):
            Packet.unpack(b"\x00" * 10)

    def test_bad_magic_rejected(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.ON, serial="", nonce=b"", payload=b"")
        wire = bytearray(pkt.pack(hmac_key=key))
        wire[0] ^= 0xFF
        with self.assertRaises(ProtocolError):
            Packet.unpack(bytes(wire))

    def test_truncated_length_field_rejected(self):
        key = bytes(range(32))
        pkt = Packet(type=PacketType.ON, serial="", nonce=b"", payload=b"1234")
        wire = pkt.pack(hmac_key=key)
        with self.assertRaises(ProtocolError):
            Packet.unpack(wire[:-1])  # cut 1 byte -- the size no longer matches

    def test_wrong_version_raises_specific_error(self):
        """A device speaking a different protocol version must be
        distinguishable from ordinary corruption."""
        key = bytes(range(32))
        pkt = Packet(type=PacketType.ON, serial="", nonce=b"", payload=b"")
        wire = bytearray(pkt.pack(hmac_key=key))
        wire[4] = 99  # version byte, right after the 4-byte magic
        with self.assertRaises(ProtocolVersionMismatchError) as ctx:
            Packet.unpack(bytes(wire))
        self.assertEqual(ctx.exception.got, 99)
        # ProtocolVersionMismatchError IS-A ProtocolError -- callers that
        # only care about "didn't parse" still catch it with the broader type.
        self.assertIsInstance(ctx.exception, ProtocolError)


class TestPacketTypes(unittest.TestCase):
    def test_change_secret_type_values(self):
        # Must stay in sync with components/admin_channel/admin_protocol.h's admin_pkt_type_t.
        self.assertEqual(PacketType.CHANGE_SECRET, 0x09)
        self.assertEqual(PacketType.CHANGE_SECRET_RESP, 0x89)

    def test_on_off_dim_type_values(self):
        self.assertEqual(PacketType.ON, 0x03)
        self.assertEqual(PacketType.ON_RESP, 0x83)
        self.assertEqual(PacketType.OFF, 0x04)
        self.assertEqual(PacketType.OFF_RESP, 0x84)
        self.assertEqual(PacketType.DIM, 0x05)
        self.assertEqual(PacketType.DIM_RESP, 0x85)

    def test_reboot_factory_reset_type_values(self):
        self.assertEqual(PacketType.REBOOT, 0x07)
        self.assertEqual(PacketType.REBOOT_RESP, 0x87)
        self.assertEqual(PacketType.FACTORY_RESET, 0x08)
        self.assertEqual(PacketType.FACTORY_RESET_RESP, 0x88)


class TestAdminStatus(unittest.TestCase):
    def test_values(self):
        # Must stay in sync with admin_status_t in admin_protocol.h.
        self.assertEqual(AdminStatus.OK, 0)
        self.assertEqual(AdminStatus.ERR_BAD_ARG, 1)
        self.assertEqual(AdminStatus.ERR_NOT_READY, 2)
        self.assertEqual(AdminStatus.ERR_INTERNAL, 3)
        self.assertEqual(AdminStatus.ACCEPTED_PENDING, 4)

    def test_status_name_known(self):
        self.assertEqual(status_name(0), "OK")
        self.assertEqual(status_name(4), "ACCEPTED_PENDING")

    def test_status_name_unknown(self):
        self.assertEqual(status_name(200), "UNKNOWN(200)")

    def test_status_byte_empty_payload(self):
        with self.assertRaises(ProtocolError):
            status_byte(b"")


class TestHmacVector(unittest.TestCase):
    def test_known_hmac_sha256_vector(self):
        # RFC 4231, Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?"
        key = b"Jefe"
        data = b"what do ya want for nothing?"
        expected = bytes.fromhex(
            "5bdcc146bf60754e6a042426089575c7" "5a003f089d2739839dec58b964ec3843"
        )
        self.assertEqual(hmac.new(key, data, hashlib.sha256).digest(), expected)


class TestInfoPayload(unittest.TestCase):
    def _build_payload(self, **overrides) -> bytes:
        fields = dict(
            mac=bytes.fromhex("A4CF12B93D08"),
            fw_version=b"1.2.3",
            ip=(192, 168, 1, 42),
            uptime_s=12345,
            reset_reason=1,
            poe_ready=1,
            poe_source=2,
            poe_cdb_confirmed=1,
            poe_t2p_confirmed=1,
            poe_vbus_confirmed=1,
            driver_on=1,
            desired_on=1,
            dim_percent=80,
            ramp_pending=0,
            vbus_mv=48000,
            led_voltage_mv=3300,
            # DMX status block
            dmx_layer_enabled=1,
            dmx_active_source=3,       # both
            dmx_level=55,
            dmx_fps=42,
            dmx_artnet_port_address=0x0102,
            dmx_sacn_universe=7,
            dmx_last_src_ip=(10, 0, 0, 5),
            dmx_address=17,
            dmx_personality=1,
            dmx_proto_mask=3,
        )
        fields.update(overrides)
        buf = bytearray()
        buf += fields["mac"]
        buf += fields["fw_version"].ljust(16, b"\x00")
        buf += bytes(fields["ip"])
        buf += struct.pack(">I", fields["uptime_s"])
        buf.append(fields["reset_reason"])
        buf.append(fields["poe_ready"])
        buf.append(fields["poe_source"])
        buf.append(fields["poe_cdb_confirmed"])
        buf.append(fields["poe_t2p_confirmed"])
        buf.append(fields["poe_vbus_confirmed"])
        buf.append(fields["driver_on"])
        buf.append(fields["desired_on"])
        buf.append(fields["dim_percent"])
        buf.append(fields["ramp_pending"])
        buf += struct.pack(">I", fields["vbus_mv"])
        buf += struct.pack(">i", fields["led_voltage_mv"])
        buf.append(fields["dmx_layer_enabled"])
        buf.append(fields["dmx_active_source"])
        buf.append(fields["dmx_level"])
        buf.append(fields["dmx_fps"])
        buf += struct.pack(">H", fields["dmx_artnet_port_address"])
        buf += struct.pack(">H", fields["dmx_sacn_universe"])
        buf += bytes(fields["dmx_last_src_ip"])
        buf += struct.pack(">H", fields["dmx_address"])
        buf.append(fields["dmx_personality"])
        buf.append(fields["dmx_proto_mask"])
        return bytes(buf)

    def test_parses_all_fields(self):
        payload = self._build_payload()
        self.assertEqual(len(payload), 64)
        parsed = parse_info_payload(payload)
        self.assertEqual(parsed["mac"], bytes.fromhex("A4CF12B93D08"))
        self.assertEqual(parsed["fw_version"], "1.2.3")
        self.assertEqual(parsed["ip"], "192.168.1.42")
        self.assertEqual(parsed["uptime_s"], 12345)
        self.assertEqual(parsed["reset_reason"], "POWERON")
        self.assertTrue(parsed["poe_ready"])
        self.assertEqual(parsed["poe_source"], "type2")
        self.assertTrue(parsed["poe_cdb_confirmed"])
        self.assertTrue(parsed["driver_on"])
        self.assertEqual(parsed["dim_percent"], 80)
        self.assertEqual(parsed["vbus_mv"], 48000)
        self.assertEqual(parsed["led_voltage_mv"], 3300)
        self.assertTrue(parsed["dmx_layer_enabled"])
        self.assertEqual(parsed["dmx_active_source"], "both")
        self.assertEqual(parsed["dmx_level"], 55)
        self.assertEqual(parsed["dmx_fps"], 42)
        self.assertEqual(parsed["dmx_artnet_port_address"], 0x0102)
        self.assertEqual(parsed["dmx_sacn_universe"], 7)
        self.assertEqual(parsed["dmx_last_src_ip"], "10.0.0.5")
        self.assertEqual(parsed["dmx_address"], 17)
        self.assertEqual(parsed["dmx_personality"], 1)
        self.assertEqual(parsed["dmx_proto_mask"], 3)

    def test_wrong_size_rejected(self):
        with self.assertRaises(ProtocolError):
            parse_info_payload(b"\x00" * 10)
        with self.assertRaises(ProtocolError):
            parse_info_payload(b"\x00" * 48)   # the pre-DMX layout is no longer accepted

    def test_unknown_enums_fall_back_to_numeric_string(self):
        payload = self._build_payload(reset_reason=250, poe_source=99)
        parsed = parse_info_payload(payload)
        self.assertEqual(parsed["reset_reason"], "250")
        self.assertEqual(parsed["poe_source"], "99")


class TestDmxConfig(unittest.TestCase):
    def test_round_trip(self):
        cfg = DmxConfig(
            layer_enabled=True,
            proto_mask=0x03,
            artnet_port_address=DmxConfig.from_artnet_parts(net=2, subnet=1, universe=5),
            sacn_universe=1234,
            dmx_address=17,
            personality=1,
            merge_mode=1,
            loss_behavior=2,
            loss_level=40,
            loss_timeout_ms=2500,
            smoothing_ms=80,
            allow_artaddress=False,
        )
        blob = pack_dmx_config(cfg)
        self.assertEqual(len(blob), DMX_CFG_WIRE_SIZE)
        self.assertEqual(parse_dmx_config(blob), cfg)

    def test_artnet_parts_views(self):
        cfg = DmxConfig(artnet_port_address=DmxConfig.from_artnet_parts(3, 4, 6))
        self.assertEqual(cfg.artnet_net, 3)
        self.assertEqual(cfg.artnet_subnet, 4)
        self.assertEqual(cfg.artnet_universe, 6)

    def test_defaults_pack(self):
        blob = pack_dmx_config(DmxConfig())
        self.assertEqual(blob[0], 1)  # layout version
        self.assertEqual(parse_dmx_config(blob), DmxConfig())

    def test_wrong_size_rejected(self):
        with self.assertRaises(ProtocolError):
            parse_dmx_config(b"\x01" * 10)


if __name__ == "__main__":
    unittest.main()
