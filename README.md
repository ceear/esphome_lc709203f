# esphome-lc709203f-deepsleep

ESPHome external component for the **LC709203F** LiPo fuel gauge, optimised
for ESP32 **deep-sleep** nodes.

---

## Why This Exists

The official ESPHome `lc709203f` component uses a 3-step state machine:
first two `update()` calls write initialisation registers, the third (and
every subsequent call) reads actual sensor data.  For always-on nodes this
is fine.  For deep-sleep nodes it creates two problems:

**Problem 1 – Dummy updates needed before data arrives**

With `update_interval: never`, you must call `component.update` three times
before you see battery voltage and level.  This breaks the simple
"wake → measure → sleep" pattern.

**Problem 2 – INITIAL_RSOC is written on every ESP32 boot**

The LC709203F stays powered from the LiPo battery at all times.  The ESP32
waking from deep sleep does not interrupt the fuel gauge's operation.
Writing `INITIAL_RSOC = 0xAA55` resets the chip's RSOC algorithm and forces
a new open-circuit-voltage sample – which may be inaccurate while WiFi is
drawing current.  Over many wake cycles, RSOC accuracy degrades rather than
improving.

### This component's solution

1. On each ESP32 wake-up, **read first**: get `CELL_VOLTAGE` and `ITE` from
   the already-running chip.
2. **Plausibility check** substitutes the missing POR indicator: if values
   are within configurable bounds (default 2.5–4.35 V, 0–100 %), the gauge
   is considered to be running correctly.
3. **INITIAL_RSOC is not written** on a valid wake-up.
4. The first `component.update` call after boot delivers real sensor values.

---

## LC709203F is Not a Coulomb Counter

The LC709203F estimates RSOC from **open-circuit voltage** and temperature –
it is not a Coulomb counter.  Unlike chips such as the MAX17055, it does not
integrate current over time.  This means:

- Accuracy is best when the battery is at rest (ESP32 in deep sleep).
- It does not track current drawn by a continuous load in real time.
- Writing `INITIAL_RSOC` repeatedly disrupts its OCV-drift calibration.

---

## Hardware Wiring

```
1S LiPo (+)
  ├──────────────► LC709203F BAT / VBAT  ← direct cell connection, not via regulator
  └──────────────► Charger + load path

1S LiPo (-)
  ├──────────────► LC709203F GND
  └──────────────► ESP32 GND / system GND

ESP32 3V3 ────────► LC709203F VDD        ← logic supply (some breakouts only)
ESP32 SDA ────────► LC709203F SDA        ← 4.7 kΩ pull-up to 3.3 V !
ESP32 SCL ────────► LC709203F SCL        ← 4.7 kΩ pull-up to 3.3 V !
```

> **Pull-up voltage warning:** Some breakout boards route I2C pull-ups to VBAT
> (up to 4.2 V).  This **exceeds** the 3.6 V absolute maximum of most ESP32
> I/O pins.  Check your breakout schematic.  If the pull-ups are on VBAT,
> remove them and add 4.7 kΩ resistors from SDA/SCL to the ESP32's 3.3 V rail.

---

## Quick Start

### 1. Reference the component

**Local development (this repo):**
```yaml
external_components:
  - source:
      type: local
      path: path/to/esphome-lc709203f-deepsleep/components
    components: [lc709203f_deepsleep]
```

**From GitHub:**
```yaml
external_components:
  - source: github://YOUR_USERNAME/esphome-lc709203f-deepsleep@main
    components: [lc709203f_deepsleep]
```

### 2. Minimal deep-sleep configuration

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22

sensor:
  - platform: lc709203f_deepsleep
    id: battery_gauge
    address: 0x0B
    battery_size: 1000          # mAh
    update_interval: never
    mode: deep_sleep
    battery_voltage:
      name: "Battery Voltage"
    battery_level:
      name: "Battery Level"

deep_sleep:
  id: deep_sleep_ctrl
  sleep_duration: 5min

esphome:
  on_boot:
    priority: -100
    then:
      - wait_until:
          condition:
            mqtt.connected:
          timeout: 30s
      - component.update: battery_gauge
      - delay: 1500ms
      - deep_sleep.enter: deep_sleep_ctrl
```

---

## Configuration Reference

```yaml
sensor:
  - platform: lc709203f_deepsleep
    address: 0x0B               # LC709203F fixed I2C address
    battery_size: 1000          # Battery capacity in mAh (100–6000)
    pack_voltage: "3.8V"        # "3.8V" (default, most LiPo) or "3.7V"
    update_interval: never      # Recommended for deep sleep; or e.g. 60s for always-on

    # Operating mode
    mode: deep_sleep            # deep_sleep (default) or normal

    # Initialization control (deep_sleep mode)
    assume_already_initialized: true   # Read existing state before deciding to init
    initialize_if_invalid: true        # Auto-init if values are implausible
    force_initialize: false            # true = full init every boot (for commissioning)

    # Fine-grained register write control
    write_initial_rsoc_on_boot: false  # Forbidden in deep_sleep mode unless intentional
    set_operational_mode_on_boot: true # Safe: wakes chip if it entered its own sleep
    set_temperature_mode_on_boot: false
    set_apa_on_boot: false             # APA persists; only needed on chip power loss

    # Plausibility thresholds
    valid_voltage_min: 2.5      # V – readings below this trigger re-init
    valid_voltage_max: 4.35     # V
    valid_rsoc_min: 0           # %
    valid_rsoc_max: 100         # %

    # Debug
    debug_registers: false      # true = dump all registers to serial on boot

    # Sensors (all optional)
    battery_voltage:
      name: "Battery Voltage"
    battery_level:
      name: "Battery Level"
    temperature:
      name: "Cell Temperature"  # Requires I2C temperature mode or NTC thermistor
```

---

## Examples

| File | Description |
|------|-------------|
| [`examples/esp32-deep-sleep.yaml`](examples/esp32-deep-sleep.yaml) | Deep-sleep node, MQTT, single update per wake-up |
| [`examples/esp32-normal.yaml`](examples/esp32-normal.yaml) | Always-on node, 60 s polling |
| [`examples/esp32-debug.yaml`](examples/esp32-debug.yaml) | Verbose logging, register dump, commissioning |

---

## Known Limitations

- **No POR indicator:** The LC709203F does not have a Power-On Reset flag.
  The plausibility check (voltage + RSOC range) is a heuristic, not a
  guaranteed detection.  An unlikely edge case exists where a corrupted chip
  state produces values that fall within the plausible range.  Set
  `force_initialize: true` for the first few boots after hardware changes.

- **OCV accuracy under load:** The RSOC reading is most accurate when the
  battery is at rest.  A deep-sleep wake-up with active WiFi introduces a
  small error.  For most use cases this is < 2 %.

- **No current tracking:** The chip cannot track coulombs consumed during
  sleep.  RSOC is inferred from voltage only.

- **Single battery cell only:** The LC709203F is designed for single-cell
  LiPo/Li-Ion only.

---

## Debugging

Enable verbose logging and register dump:

```yaml
logger:
  level: VERBOSE

sensor:
  - platform: lc709203f_deepsleep
    debug_registers: true
    ...
```

Expected boot log for a valid deep-sleep wake-up:
```
[lc709203f_ds] I2C device found at 0x0B, IC version: 0x0000
[lc709203f_ds] Power mode: operational
[lc709203f_ds] Read: voltage=3.882 V, RSOC=55.1 %
[lc709203f_ds] Existing gauge state considered valid; skipping Initial RSOC
[lc709203f_ds] Voltage read: 3.882 V
[lc709203f_ds] RSOC read: 55.1 %
[lc709203f_ds] Published battery values
```

---

## Differences from MAX17055

The MAX17055 has an explicit `POR` bit in its `STATUS` register that clearly
indicates whether a full learned-model initialisation is needed.  This allows
precise detection of power-on vs. host-restart scenarios.

The LC709203F provides no equivalent mechanism.  This component uses a
voltage+RSOC plausibility check as a substitute.  See
[`docs/initialization.md`](docs/initialization.md) for the full decision logic.

---

## Sources and References

- [ESPHome LC709203F component source](https://github.com/esphome/esphome/tree/dev/esphome/components/lc709203f)
- [ESPHome external_components documentation](https://esphome.io/components/external_components)
- [LC709203F datasheet – ON Semiconductor / ABLIC](https://www.onsemi.com/pdf/datasheet/lc709203f-d.pdf)
- [LC709203F Application Note – ABLIC](https://www.ablic.com/en/semicon/products/battery-management-ic/battery-fuel-gauge-ic/lc709203f/)
- [Adafruit LC709203F library](https://github.com/adafruit/Adafruit_LC709203F)
- [Adafruit LC709203F breakout](https://learn.adafruit.com/adafruit-lc709203f-lipo-lipoly-battery-monitor)
- [ESPHome deep_sleep component](https://esphome.io/components/deep_sleep)
- [ESPHome component.update action](https://esphome.io/guides/automations#component-update-action)

---

## License

MIT License – see [`LICENSE`](LICENSE).

This component is an independent implementation.  It shares no source code
with the ESPHome `lc709203f` component (which is Apache 2.0 licensed), but
was informed by studying its behaviour and register usage.
