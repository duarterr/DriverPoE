"""Typed data models returned by client.py/discovery.py. No I/O, no
protocol knowledge beyond what's needed to build these from already-parsed
fields.
"""
from __future__ import annotations

from dataclasses import dataclass

from .protocol import AdminStatus, mac_to_str


@dataclass(frozen=True)
class DeviceInfo:
    """One unit's INFO_RESP, fully parsed and typed. See
    protocol.parse_info_payload() for the wire layout this mirrors."""
    serial: str
    source_ip: str  # address the response actually came from (socket-level, not the payload's own 'ip' field)
    mac: bytes
    fw_version: str
    ip: str
    uptime_s: int
    reset_reason: str
    poe_ready: bool
    poe_source: str
    poe_cdb_confirmed: bool
    poe_t2p_confirmed: bool
    poe_vbus_confirmed: bool
    driver_on: bool
    desired_on: bool
    dim_percent: int
    vbus_mv: int
    led_voltage_mv: int
    # DMX layer status (from the INFO response's trailing status block).
    dmx_layer_enabled: bool
    dmx_active_source: str            # none | artnet | sacn | both
    dmx_level: int                   # 0..100 currently applied by the DMX layer
    dmx_fps: int
    dmx_artnet_port_address: int
    dmx_sacn_universe: int
    dmx_last_src_ip: str
    dmx_address: int                 # DMX start channel, 1..512
    dmx_personality: int             # 0 = 1ch 8-bit, 1 = 2ch 16-bit
    dmx_proto_mask: int              # DMX_PROTO_ARTNET | DMX_PROTO_SACN
    # HV9910 dimming mode (from the INFO response's trailing block).
    dimming_mode: int                # 0 = pwm, 1 = analog, 2 = hybrid
    dimming_pwm_freq_hz: int
    dimming_analog_freq_hz: int
    dimming_min_on_time_us: int
    dimming_crossover_pct: int
    # Power policy + live state (from the INFO response's trailing power block).
    power_mode: int                  # 0 = auto, 1 = poe_only, 2 = poe_plus_required
    power_mode_name: str
    poe_cap_pct: int                 # LD-reference scale under the Type-1 cap
    power_state: int                 # 0 = no_power, 1 = full, 2 = capped, 3 = blocked
    power_state_name: str
    power_effective_scale_pct: int   # LD scale currently applied (100 or the cap)
    power_budget_w: float            # negotiated PoE budget: 0 / 12.95 / 25.5

    @property
    def mac_str(self) -> str:
        return mac_to_str(self.mac)

    @property
    def power_blocking_reason(self) -> str | None:
        """None if power is ready; otherwise which PoE negotiation gate is
        still blocking it (see components/admin_channel/admin_channel.c
        handle_info()'s comment) -- useful for a UI to explain why a unit
        that's reachable over the network still won't turn its LED on."""
        if self.poe_ready:
            return None
        if not self.poe_cdb_confirmed:
            return "waiting for PoE detection (CDB)"
        if not self.poe_t2p_confirmed:
            return "waiting for PoE classification (T2P)"
        if not self.poe_vbus_confirmed:
            return "VBUS below the minimum threshold"
        return "not ready (unknown reason)"


@dataclass(frozen=True)
class CommandResult:
    """Outcome of one write command (ON/OFF/DIM/IDENTIFY/REBOOT/
    FACTORY_RESET/CHANGE_SECRET/DMX_SET_CONFIG). Distinguishes "accepted"
    from "applied" -- ADMIN_STATUS_ACCEPTED_PENDING means the device
    validated and recorded the request but couldn't apply it yet (power
    not confirmed); it is NOT an error, so this never gets raised as an
    exception on its own -- see raise_if_refused()."""
    status: AdminStatus
    serial: str
    command: str

    @property
    def accepted(self) -> bool:
        """True if the device validated the request at all -- OK or
        ACCEPTED_PENDING, false for any ERR_* status."""
        return self.status in (AdminStatus.OK, AdminStatus.ACCEPTED_PENDING)

    @property
    def applied(self) -> bool:
        """True only if the device applied the request immediately."""
        return self.status == AdminStatus.OK

    @property
    def pending(self) -> bool:
        """True if the request was accepted but deferred (see
        DeviceInfo.power_blocking_reason for why)."""
        return self.status == AdminStatus.ACCEPTED_PENDING

    def raise_if_refused(self) -> "CommandResult":
        """Convenience for callers that want exception-style handling
        instead of checking .accepted themselves. Returns self so it can
        be chained: `result = client.on(...).raise_if_refused()`."""
        if not self.accepted:
            from .client import CommandRefusedError  # local import: avoids a cycle with client.py
            raise CommandRefusedError(self.status, self.command, self.serial)
        return self
