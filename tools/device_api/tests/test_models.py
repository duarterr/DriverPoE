"""Tests for device_api.models -- no network involved."""
from __future__ import annotations

import unittest

from device_api.client import CommandRefusedError
from device_api.models import CommandResult, DeviceInfo
from device_api.protocol import AdminStatus


def _info(**overrides) -> DeviceInfo:
    fields = dict(
        serial="DriverPoE-A4CF12B93D08",
        source_ip="10.0.0.5",
        mac=bytes.fromhex("A4CF12B93D08"),
        fw_version="1.0.0",
        ip="10.0.0.5",
        uptime_s=100,
        reset_reason="POWERON",
        poe_ready=True,
        poe_source="type2",
        poe_cdb_confirmed=True,
        poe_t2p_confirmed=True,
        poe_vbus_confirmed=True,
        driver_on=True,
        desired_on=True,
        dim_percent=80,
        ramp_pending=False,
        vbus_mv=48000,
        led_voltage_mv=3300,
        dmx_layer_enabled=False,
        dmx_active_source="none",
        dmx_level=0,
        dmx_fps=0,
        dmx_artnet_port_address=0,
        dmx_sacn_universe=1,
        dmx_last_src_ip="0.0.0.0",
        dmx_address=1,
        dmx_personality=0,
        dmx_proto_mask=3,
        dimming_mode=2,
        dimming_pwm_freq_hz=2000,
        dimming_analog_freq_hz=60000,
        dimming_min_on_time_us=20,
        dimming_crossover_pct=20,
    )
    fields.update(overrides)
    return DeviceInfo(**fields)


class TestDeviceInfo(unittest.TestCase):
    def test_mac_str(self):
        self.assertEqual(_info().mac_str, "A4:CF:12:B9:3D:08")

    def test_power_blocking_reason_none_when_ready(self):
        self.assertIsNone(_info(poe_ready=True).power_blocking_reason)

    def test_power_blocking_reason_cdb(self):
        info = _info(poe_ready=False, poe_cdb_confirmed=False, poe_t2p_confirmed=False, poe_vbus_confirmed=False)
        self.assertIn("CDB", info.power_blocking_reason)

    def test_power_blocking_reason_t2p(self):
        info = _info(poe_ready=False, poe_cdb_confirmed=True, poe_t2p_confirmed=False, poe_vbus_confirmed=False)
        self.assertIn("T2P", info.power_blocking_reason)

    def test_power_blocking_reason_vbus(self):
        info = _info(poe_ready=False, poe_cdb_confirmed=True, poe_t2p_confirmed=True, poe_vbus_confirmed=False)
        self.assertIn("VBUS", info.power_blocking_reason)


class TestCommandResult(unittest.TestCase):
    def test_ok_is_accepted_and_applied(self):
        r = CommandResult(status=AdminStatus.OK, serial="S1", command="ON")
        self.assertTrue(r.accepted)
        self.assertTrue(r.applied)
        self.assertFalse(r.pending)
        self.assertEqual(r.raise_if_refused(), r)

    def test_accepted_pending_is_accepted_but_not_applied(self):
        r = CommandResult(status=AdminStatus.ACCEPTED_PENDING, serial="S1", command="ON")
        self.assertTrue(r.accepted)
        self.assertFalse(r.applied)
        self.assertTrue(r.pending)
        r.raise_if_refused()  # must NOT raise -- pending is not an error

    def test_error_status_is_not_accepted(self):
        for status in (AdminStatus.ERR_BAD_ARG, AdminStatus.ERR_NOT_READY, AdminStatus.ERR_INTERNAL):
            r = CommandResult(status=status, serial="S1", command="DIM")
            self.assertFalse(r.accepted)
            self.assertFalse(r.applied)
            self.assertFalse(r.pending)
            with self.assertRaises(CommandRefusedError):
                r.raise_if_refused()


if __name__ == "__main__":
    unittest.main()
