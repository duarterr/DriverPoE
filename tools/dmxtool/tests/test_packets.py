"""Wire-format checks for dmxtool's Art-Net / sACN packet builders.

These mirror the offsets components/dmx_input/dmx_artnet.c and
components/dmx_input/dmx_sacn.c parse -- if the firmware layout changes,
these should change with it.
"""
from __future__ import annotations

import os
import struct
import unittest

from dmxtool.__main__ import artnet_packet, sacn_packet


class TestArtNet(unittest.TestCase):
    def test_artdmx_layout(self):
        slots = bytes([0, 0, 200, 0])  # channel 3 at 200
        pkt = artnet_packet(seq=7, port_address=0x0102, slots=slots)
        self.assertEqual(pkt[0:8], b"Art-Net\x00")
        self.assertEqual(struct.unpack("<H", pkt[8:10])[0], 0x5000)
        self.assertEqual(pkt[10], 0)
        self.assertEqual(pkt[11], 14)
        self.assertEqual(pkt[12], 7)                 # sequence
        self.assertEqual(pkt[14], 0x02)             # SubUni (low byte of port address)
        self.assertEqual(pkt[15], 0x01)             # Net
        length = struct.unpack(">H", pkt[16:18])[0]
        self.assertEqual(length, 4)
        self.assertEqual(pkt[18:18 + length], slots)

    def test_odd_length_padded_even(self):
        pkt = artnet_packet(seq=0, port_address=0, slots=bytes([1, 2, 3]))
        self.assertEqual(struct.unpack(">H", pkt[16:18])[0], 4)
        self.assertEqual(len(pkt) - 18, 4)


class TestSacn(unittest.TestCase):
    def test_data_packet_layout(self):
        cid = os.urandom(16)
        slots = bytes([0] * 2 + [255])  # channel 3 full
        pkt = sacn_packet(cid, seq=42, universe=7, slots=slots, priority=100, terminated=False)
        self.assertEqual(pkt[4:16], b"ASC-E1.17\x00\x00\x00")
        self.assertEqual(struct.unpack(">I", pkt[18:22])[0], 0x00000004)  # root vector
        self.assertEqual(pkt[22:38], cid)
        self.assertEqual(struct.unpack(">I", pkt[40:44])[0], 0x00000002)  # framing vector
        self.assertEqual(pkt[108], 100)             # priority
        self.assertEqual(pkt[111], 42)             # sequence
        self.assertEqual(pkt[112], 0x00)           # options
        self.assertEqual(struct.unpack(">H", pkt[113:115])[0], 7)  # universe
        self.assertEqual(pkt[117], 0x02)           # DMP vector
        prop_count = struct.unpack(">H", pkt[123:125])[0]
        self.assertEqual(prop_count, len(slots) + 1)
        self.assertEqual(pkt[125], 0x00)           # start code
        self.assertEqual(pkt[126:126 + len(slots)], slots)

    def test_terminated_sets_option_bit(self):
        pkt = sacn_packet(os.urandom(16), seq=1, universe=1, slots=b"\x00",
                          priority=100, terminated=True)
        self.assertEqual(pkt[112] & 0x40, 0x40)


if __name__ == "__main__":
    unittest.main()
