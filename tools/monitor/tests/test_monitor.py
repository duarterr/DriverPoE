"""Tests for monitor's anomaly detection -- no network involved. Feeds
DeviceState.observe() a scripted sequence of DeviceInfo/None snapshots and
checks which anomalies come out."""
from __future__ import annotations

import unittest

from device_api.models import DeviceInfo
from monitor.__main__ import DeviceState, diff_snapshots


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
        power_mode=0,
        power_mode_name="auto",
        poe_cap_pct=51,
        power_state=1,
        power_state_name="full",
        power_effective_scale_pct=100,
        power_budget_w=25.5,
        lin_enable=True,
    )
    fields.update(overrides)
    return DeviceInfo(**fields)


class TestDiffSnapshots(unittest.TestCase):
    def test_no_change_is_silent(self):
        a = _info()
        b = _info()
        self.assertEqual(diff_snapshots("S", "ip", a, b), [])

    def test_reboot_detected_from_uptime_going_backwards(self):
        prev = _info(uptime_s=500)
        cur = _info(uptime_s=3, reset_reason="BROWNOUT")
        events = diff_snapshots("S", "ip", prev, cur)
        kinds = [e.kind for e in events]
        self.assertIn("reboot", kinds)
        reboot = next(e for e in events if e.kind == "reboot")
        self.assertEqual(reboot.severity, "crit")
        self.assertIn("BROWNOUT", reboot.message)

    def test_poe_lost_then_recovered(self):
        prev = _info(poe_ready=True, vbus_mv=48000)
        # vbus_mv=0 is deliberately below the vbus_sag comparison's own
        # guard (both sides > 0) -- poe_lost's message already carries the
        # before/after mV, so this must not also double-report vbus_sag.
        cur = _info(poe_ready=False, poe_vbus_confirmed=False, vbus_mv=0)
        events = diff_snapshots("S", "ip", prev, cur)
        self.assertEqual([e.kind for e in events], ["poe_lost"])
        self.assertEqual(events[0].severity, "crit")

        events2 = diff_snapshots("S", "ip", cur, prev)
        self.assertIn("poe_recovered", [e.kind for e in events2])

    def test_power_state_downgrade_is_warn_upgrade_is_info(self):
        full = _info(power_state_name="full", power_budget_w=25.5)
        blocked = _info(power_state_name="blocked", power_budget_w=0.0)

        down = diff_snapshots("S", "ip", full, blocked)
        change = next(e for e in down if e.kind == "power_state_change")
        self.assertEqual(change.severity, "warn")

        up = diff_snapshots("S", "ip", blocked, full)
        change2 = next(e for e in up if e.kind == "power_state_change")
        self.assertEqual(change2.severity, "info")

    def test_led_dark_while_still_commanded_on(self):
        prev = _info(desired_on=True, driver_on=True, dim_percent=80)
        cur = _info(desired_on=True, driver_on=False, dim_percent=80)
        events = diff_snapshots("S", "ip", prev, cur)
        dark = next(e for e in events if e.kind == "led_dark")
        self.assertEqual(dark.severity, "crit")
        self.assertIn("80%", dark.message)

    def test_led_off_because_we_asked_is_not_an_anomaly(self):
        prev = _info(desired_on=True, driver_on=True)
        cur = _info(desired_on=False, driver_on=False)
        events = diff_snapshots("S", "ip", prev, cur)
        self.assertNotIn("led_dark", [e.kind for e in events])

    def test_vbus_sag_below_threshold_is_not_flagged(self):
        prev = _info(vbus_mv=48000)
        cur = _info(vbus_mv=47000)  # ~2%, under the 10% default
        events = diff_snapshots("S", "ip", prev, cur)
        self.assertNotIn("vbus_sag", [e.kind for e in events])

    def test_vbus_sag_above_threshold_flagged_even_without_losing_poe_ready(self):
        # A brief renegotiation dip that never actually drops poe_ready --
        # the case that would otherwise go completely unnoticed.
        prev = _info(vbus_mv=48000, poe_ready=True)
        cur = _info(vbus_mv=40000, poe_ready=True)  # ~17% sag
        events = diff_snapshots("S", "ip", prev, cur)
        sag = next(e for e in events if e.kind == "vbus_sag")
        self.assertEqual(sag.severity, "warn")
        self.assertIn("without losing poe_ready", sag.message)

    def test_fw_change_reported(self):
        prev = _info(fw_version="1.0.0")
        cur = _info(fw_version="1.0.1")
        events = diff_snapshots("S", "ip", prev, cur)
        self.assertIn("fw_changed", [e.kind for e in events])


class TestDeviceState(unittest.TestCase):
    def test_first_sighting_emits_seen_not_a_diff(self):
        st = DeviceState("10.0.0.5")
        events = st.observe(_info(), now=0.0)
        self.assertEqual([e.kind for e in events], ["seen"])

    def test_unreachable_then_reachable_reports_outage_length(self):
        st = DeviceState("10.0.0.5")
        st.observe(_info(), now=0.0)

        events = st.observe(None, now=1.0)
        self.assertEqual([e.kind for e in events], ["unreachable"])

        events = st.observe(None, now=2.0)  # still down -- no repeated event
        self.assertEqual(events, [])

        events = st.observe(_info(uptime_s=50, reset_reason="TASK_WDT"), now=9.0)
        kinds = [e.kind for e in events]
        self.assertEqual(kinds[0], "reachable")
        self.assertIn("8.0s", events[0].message)
        self.assertIn("reboot", kinds)  # uptime reset to 50 after being 100

    def test_full_scenario_reboot_then_recovery(self):
        """Mirrors the reported symptom: LED left on, then dark, and PoE
        renegotiates -- across polls that should read as one reboot."""
        st = DeviceState("10.0.0.5")
        st.observe(_info(uptime_s=1000, poe_ready=True, driver_on=True, desired_on=True), now=0.0)

        # mid-reboot: unit is briefly gone
        self.assertEqual([e.kind for e in st.observe(None, now=1.0)], ["unreachable"])

        # back up: fresh boot, PoE re-negotiated, LED not yet re-applied
        events = st.observe(_info(uptime_s=2, reset_reason="BROWNOUT", poe_ready=True,
                                   driver_on=False, desired_on=True, vbus_mv=48000), now=6.0)
        kinds = {e.kind for e in events}
        self.assertEqual(kinds, {"reachable", "reboot", "led_dark"})


if __name__ == "__main__":
    unittest.main()
