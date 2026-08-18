| Supported Targets | ESP32 |
| ----------------- | ----- |

# PoE Luminaire — minimal firmware

Minimal firmware for a PoE-powered luminaire, built on:

- **ESP32** (internal EMAC)
- **IP101G** — Ethernet PHY over RMII
- **TPS2378** — PoE negotiator/interface (IEEE 802.3at, PD)
- **HV9910** — LED driver (buck, PWM dimming)

## Behavior

1. At boot, before any other initialization, the HV9910 LED driver is
   forced into the disabled state (`hv9910_init()`).
2. The ADC (`voltage_sense_init()`) comes up next, so VBUS is already
   readable once the PoE/AUX gate starts evaluating it.
3. The firmware starts monitoring the TPS2378's **CDB** and **T2P** pins,
   cross-checked against the measured DC bus voltage (**VBUS**, read via
   `ADC_VLED_P`). The system is considered "ready" (safe to operate) when
   **both**:
   - A digital source is confirmed — **either**:
     - **PoE OK** — CDB confirms a real PoE source has been negotiated and
       inrush is done (true for both Type-1/802.3af and Type-2/802.3at), or
     - **AUX present** — T2P is asserted, which happens either on real
       Type-2 classification, or because this board's AUX bench-power
       input (a direct DC injection onto the downstream bus, for testing
       without a real PoE source) is above ~40V and forces the TPS2378's
       APD pin high.
   - **AND VBUS backs it up** — the measured DC bus voltage is above
     `VBUS_MIN_MV` (default 40V, `main/board_pins.h`). CDB/T2P are just
     digital handshake signals; VBUS is the final cross-check that the
     rail is actually healthy before the LED driver is trusted to run.

   While **either** requirement fails ("low-power mode"), **indicator LED
   2 (red, GPIO12)** blinks and the LED driver stays locked out —
   including if the combined condition drops again after having been OK
   (power loss, AUX removed, VBUS sagging, thermal overload,
   renegotiation), the firmware cuts the driver immediately. See
   [main/poe_negotiator.h](main/poe_negotiator.h) for the full logic and
   reasoning.
4. As soon as the system becomes ready, the firmware:
   - initializes the IP101G PHY and brings up the Ethernet stack (DHCP
     client by default, via `esp_netif`/lwIP);
   - starts a TCP command server (configurable port, default 5000).
5. From then on, commands received over the network can turn the driver
   on/off, adjust brightness, and read voltages/state.

See [main/board_pins.h](main/board_pins.h) for the full pin map and for
**every application configuration parameter** (TCP port, HV9910
polarities, PWM frequency, PHY address, voltage-divider ratio) — all as
`#define`s, intentionally with nothing in Kconfig/menuconfig. To change
any of these values, edit `board_pins.h` and rebuild.

## ⚠️ Assumptions that still need bench validation

These polarity assumptions couldn't be confirmed without the full
schematic — the firmware assumes the "safe side" described below, but
**confirm with a multimeter before powering the LEDs**:

- **GPIO12 (indicator LED 2)** is an ESP32 strapping pin (MTDI, selects
  the flash voltage at reset). Test boot with the LED connected before
  considering the hardware validated.
- **GPIO2 (T2P)** and **GPIO0 (PHY clock)** are also strapping pins (boot
  mode). The pattern used here (software pull-up on T2P, enabled at
  runtime after the boot-mode strapping window; IP101G generating the
  50MHz clock on GPIO0) is the same general one used in Espressif's own
  RMII examples, but test a cold power-on (not just a reset) before
  trusting it.

**Already confirmed, no need to validate**:
- **SHUTDOWN (GPIO32)** is direct/non-inverted logic: GPIO **HIGH = driver
  ENABLED**, LOW = disabled (`HV9910_SHUTDOWN_ACTIVE_HIGH=0` in
  `main/board_pins.h`) — confirmed on the bench via the `ON`/`OFF`
  commands (the opposite setting had `ON` turning the driver off and vice
  versa).
- **DIMMING (GPIO33)** does have an external inverting stage between the
  GPIO and the HV9910's PWMD pin (`HV9910_DIM_ACTIVE_HIGH=1` in
  `main/board_pins.h`); the LEDC driver compensates for this via
  `output_invert`, so `DIM 0` means "off" and `DIM 100` means max
  brightness from the protocol user's point of view — confirmed on the
  bench via the `DIM` command (the opposite setting had `DIM 99` produce
  ~1% brightness and `DIM 1` produce ~99%).
- **CDB and T2P (GPIO15 and GPIO2)** have no external pull-up resistor on
  this board — both are open-drain outputs on the TPS2378, so the ESP32's
  internal software pull-up (enabled in `poe_negotiator_init()`) is what
  holds them HIGH when the TPS2378 isn't actively driving them low.
- The IP101G's SMI address (`ETH_PHY_ADDR=1` in `main/board_pins.h`) was
  calculated from the actual strap (PHY_AD0 pulled up, PHY_AD3 pulled
  down) using Table 4 ("PHY Address Configuration") of the IC Plus
  IP101G-DS-R01 datasheet — see the comment in the header itself.
- The `ADC_VLED_P`/`ADC_VLED_N` voltage-divider ratio (`VLED_DIVIDER_RATIO`
  in `main/board_pins.h`) is computed from the actual resistor values
  (Rup=560k, Rdown=22k): `(Rup+Rdown)/Rdown ≈ 26.45`. ADC_VLED_P is tapped
  directly off the DC bus, so it doubles as the bus voltage sense (VBUS);
  the reported LED voltage is `VBUS - LEDVN` (see `vbus_mv` and
  `led_voltage_mv` in the `STATUS` command below — the individual
  LEDVP/LEDVN pin readings aren't exposed, only the two values that
  actually matter).

## PoE / AUX power reporting

Once ready, the firmware logs and reports (via `STATUS`, field
`poe_source`) which digital condition was satisfied, and — for the two
real-PoE sources — the guaranteed power available at the PD, using the
standard IEEE 802.3af/at headline figures. This is **not** a live power
measurement:

| `poe_source` | Meaning                                    | `poe_power_w`        |
| ------------- | ------------------------------------------- | ---------------------|
| `none`        | Low-power mode: no digital source confirmed, or VBUS too low | `0.00` |
| `type1`       | Real PoE negotiated, IEEE 802.3af           | `12.95`               |
| `type2`       | Real PoE negotiated, IEEE 802.3at (Type-2)  | `25.50`               |
| `aux`         | AUX bench supply present, CDB not confirmed | `0.00` (not a real PoE budget) |

Note that `type2` can't be distinguished in software from "Type-1 PoE with
the AUX supply also connected" — both leave CDB confirmed (real PoE) and
T2P asserted (AUX forcing APD). Either way it's safe to operate, which is
all the firmware needs to know; the reported class in that overlap case is
just a best-effort label, not a guarantee of real 802.3at classification.

`poe_source` reflects the digital handshake only — `poe_ready` additionally
requires VBUS to be above `VBUS_MIN_MV` (`vbus_ok=1` in `STATUS`). It's
possible to see a non-`none` `poe_source` together with `poe_ready=0`: that
means CDB/T2P claim power is present but the measured bus voltage doesn't
back it up (sagging rail, wiring fault, etc) — check `vbus_mv` in that
case.

## Command protocol (TCP, text)

One text line per command, terminated by `\n` (an optional `\r` right
before it is ignored). Connect with `netcat`/`telnet` on the configured
port (default `5000`):

```
$ nc <luminaire-ip> 5000
PING
OK PONG
STATUS
OK STATUS poe_ready=1 poe_source=type2 poe_power_w=25.50 cdb_raw=1 cdb_ok=1 t2p_raw=1 t2p_ok=1 vbus_ok=1 driver_on=0 dim=0 dim_last=50 vbus_mv=48200 led_voltage_mv=3100 eth_ip=192.168.1.50 uptime_s=42
ON
OK ON
DIM 50
OK DIM 50
DIM 0
OK DIM 0
OFF
OK OFF
```

| Command       | Effect                                                                |
| ------------- | ---------------------------------------------------------------------|
| `PING`        | `OK PONG`                                                             |
| `HELP`        | List of commands                                                      |
| `STATUS`      | Current state: PoE/AUX source and power, raw+debounced CDB/T2P/VBUS, driver, dimming, voltages, IP |
| `ON`          | Enables the HV9910, resuming the last brightness (refused with `ERR POE_NOT_READY` unless PoE or AUX is confirmed) |
| `OFF`         | Disables the HV9910                                                   |
| `DIM <0-100>` | Sets the brightness (%). `DIM 0` also disables the driver if it was on; `DIM >0` also enables it if it was off (same PoE/AUX gate as `ON`) |

`STATUS` fields explained:

| Field | Meaning |
| ----- | ------- |
| `cdb_raw` / `t2p_raw` | Instantaneous, un-debounced GPIO reads — can show transient glitches |
| `cdb_ok` / `t2p_ok`   | Debounced (confirmed) values — what `poe_source` is actually based on |
| `vbus_ok`             | Debounced VBUS-above-threshold verdict — `poe_ready = (cdb_ok or t2p_ok) and vbus_ok` |
| `vbus_mv`             | Live DC bus voltage reading, in mV (= LEDVP scaled) — compare against `VBUS_MIN_MV` in `main/board_pins.h` |
| `dim`                 | Current live brightness (0 while the driver is off) |
| `dim_last`            | Remembered brightness that `ON`/`DIM >0` will resume, persisted in NVS |

The server handles one connection at a time (minimal, on purpose).

## Brightness persistence

The last non-zero brightness is saved to NVS (namespace `hv9910`, key
`dim`) every time it changes via the `DIM` command, and reloaded at boot
(a fresh device with nothing ever saved defaults to 100%). `hv9910_enable()`
— called by both the `ON` command and by `DIM >0` on a currently-off
driver — always reapplies that remembered brightness before releasing
SHUTDOWN, so the driver never comes back on dark. See
[main/hv9910.h](main/hv9910.h) for details. This uses the `nvs` partition
already declared in [partitions.csv](partitions.csv).

## Configuration and build

All application configuration lives in
[main/board_pins.h](main/board_pins.h) (pins, TCP port, HV9910
polarities, PHY address, voltage-divider ratio) — there's nothing to
adjust in `idf.py menuconfig`. The entries in
[sdkconfig.defaults](sdkconfig.defaults) are ESP-IDF's own switches: they
enable the internal EMAC driver compilation, quiet down the default
console log verbosity, and declare the real flash size/partition table
(`CONFIG_ETH_ENABLED`, `CONFIG_ETH_USE_ESP32_EMAC`,
`CONFIG_LOG_DEFAULT_LEVEL_WARN`, `CONFIG_LOG_MAXIMUM_LEVEL_INFO`,
`CONFIG_BOOTLOADER_LOG_LEVEL_WARN`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`,
`CONFIG_PARTITION_TABLE_CUSTOM`) — not hardware parameters of the
project.

### Flash size / partition table

This board has a 4MB flash chip. [partitions.csv](partitions.csv) at the
project root declares a partition table sized for it (same `nvs`/`phy_init`
layout as ESP-IDF's default single-app table, just with a bigger `factory`
app partition — no OTA partitions, this is a single-image firmware).
`sdkconfig.defaults` points the build at it and declares the matching 4MB
flash size, which also fixes the `Detected size(4096k) larger than the
size in the binary image header(2048k)` boot warning that shows up if the
declared size doesn't match the real chip.

```
idf.py set-target esp32
idf.py -p PORT flash monitor
```

The first time, the IDF component manager will download the IP101G PHY
driver (`espressif/ip101`, see
[main/idf_component.yml](main/idf_component.yml)) — internet access is
required for that first build.

## Console log verbosity

By default, ESP-IDF prints a lot of startup noise (bootloader banner,
partition table dump, image segment loading, heap/flash probing, etc)
before `app_main()` even runs. This project silences all of that via
`sdkconfig.defaults` (app default log level raised to `WARN`, bootloader
log level raised to `WARN`) and then explicitly re-enables `INFO` logging
for its own six module tags at the very start of `app_main()` — see
`quiet_boot_noise()` in
[main/poe_luminaire_main.c](main/poe_luminaire_main.c). IDF-internal
warnings/errors still print; only the routine INFO-level chatter is
suppressed.

## PoE/AUX/VBUS event logging

Every time the debounced CDB, T2P, or VBUS-above-threshold signal changes
state, `poe_negotiator` logs it immediately and independently, regardless
of whether it flips the overall ready/not-ready verdict:

```
I (...) POE_NEG: EVENT: CDB asserted — real PoE negotiated (inrush done)
W (...) POE_NEG: EVENT: T2P dropped — no AUX/Type-2 confirmation anymore
W (...) POE_NEG: EVENT: VBUS too low — 18300mV < 40000mV threshold
```

This matters because the three conditions are independent
(`ready = (PoE OK or AUX present) and VBUS ok`) — e.g. real PoE can drop
out while the bench AUX supply is still holding the system up, or CDB/T2P
can both check out while VBUS itself is sagging below threshold, and
either case is worth its own log line even when nothing else changes. On
top of that, the combined verdict itself is logged on every transition
(`READY: ...` / `LOW POWER MODE: ...`, and the latter explicitly says
whether the cause was "no digital source" or "digital source OK but VBUS
too low"), plus a reminder line prints every 5s while stuck in low-power
mode so the console never goes quiet for long during bring-up.

## Code structure

| File                          | Responsibility                                                  |
| ------------------------------ | ---------------------------------------------------------------|
| `main/board_pins.h`            | Pin map, hardware assumptions, and all app configuration values |
| `main/hv9910.[ch]`             | LED driver control (SHUTDOWN + dimming PWM), NVS brightness persistence — log tag `HV9910` |
| `main/poe_negotiator.[ch]`     | TPS2378 CDB/T2P (PoE/AUX) and VBUS monitoring, safety watchdog — log tag `POE_NEG` |
| `main/voltage_sense.[ch]`      | VBUS/VLED voltage reading (ADC_VLED_P/ADC_VLED_N) — log tag `VOLT_SENSE` |
| `main/eth_init.[ch]`           | IP101G PHY / Ethernet / DHCP bring-up — log tag `ETH_INIT`       |
| `main/cmd_server.[ch]`         | TCP text command server — log tag `CMD_SRV`                      |
| `main/poe_luminaire_main.c`    | `app_main()`: orchestrates the boot sequence above — log tag `MAIN` |
