# Deep Sleep Best Practices

## Hardware Requirements

### LC709203F power supply

The LC709203F **must** be powered directly from the LiPo cell, not from a
regulator or boost converter.  The chip measures voltage and temperature from
the raw cell terminals.

```
1S LiPo (+)
  ├──────────────► LC709203F BAT / VBAT
  └──────────────► Charger / regulator / load path

1S LiPo (-)
  ├──────────────► LC709203F GND
  └──────────────► ESP32 GND / system GND

ESP32 3V3 ────────► LC709203F VDD  (logic supply on breakouts that have one)
ESP32 SDA ────────► LC709203F SDA  (4.7 kΩ pull-up to 3.3 V)
ESP32 SCL ────────► LC709203F SCL  (4.7 kΩ pull-up to 3.3 V)
```

### I2C pull-up voltage warning

Some breakout boards (including the Adafruit LC709203F breakout before Rev C)
connect I2C pull-ups to VBAT rather than 3.3 V.  At a full LiPo charge (4.2 V)
this pulls SDA and SCL to 4.2 V, which **exceeds the 3.6 V absolute maximum**
of most ESP32 I/O pins.

**Verify your breakout's pull-up rail.  If in doubt, add your own 4.7 kΩ
pull-ups from SDA/SCL to 3.3 V and cut / remove the board's existing ones.**

### LiPo protection

The LC709203F does not protect the battery from over-discharge.  Use a battery
with built-in protection circuitry (PCM/BMS) or add external protection.

---

## ESP32 Deep Sleep Wiring

The LC709203F stays powered even when the ESP32 is in deep sleep because it
draws power from VBAT, not from the ESP32.  No special circuit is needed.

If you use the `deep_sleep` component's `wakeup_pin` option, make sure the
GPIO you use is an RTC-capable pin and has appropriate pull-up/pull-down.

---

## ESPHome YAML Pattern

```yaml
deep_sleep:
  id: deep_sleep_ctrl
  sleep_duration: 5min   # Adjust to your measurement frequency

esphome:
  on_boot:
    priority: -100        # After all components initialised
    then:
      - wait_until:
          condition:
            mqtt.connected:   # or api.connected for native API
          timeout: 30s
      - component.update: battery_gauge   # single call is enough
      - delay: 1500ms                     # allow MQTT publish to complete
      - deep_sleep.enter: deep_sleep_ctrl
```

Key points:
- `update_interval: never` on the sensor – only triggered manually
- One `component.update` call gives real data (no warm-up needed)
- `delay` before `deep_sleep.enter` gives the transport layer time to flush

---

## Choosing Sleep Duration

The LC709203F's RSOC accuracy is best when the battery is **at rest** during
the OCV measurement.  At the moment `component.update` fires, the ESP32 has
been running (WiFi active) for a few seconds, drawing some current.

For best accuracy:
1. Enter deep sleep first, let WiFi and ESP32 stop drawing current.
2. Let the battery voltage recover for at least the LC709203F's settling time
   (instantaneous in practice – the OCV recovery after WiFi current draw is
   fast on most LiPo cells).
3. The measurement at the next wake-up reflects the true resting OCV.

In practice, the error introduced by measuring during active WiFi draw is small
(< 1–2 %) for most sleep durations > 30 seconds.

---

## First Boot / Commissioning

On first power-on, the chip will have zero or invalid register values.
The component detects this via the plausibility check and automatically runs
initialisation (because `initialize_if_invalid: true` is the default).

After the first successful init, the chip retains its calibration state
across all subsequent ESP32 deep-sleep cycles as long as the LiPo remains
connected.

To deliberately force a full re-init (e.g. after replacing the battery),
temporarily set `force_initialize: true` in the YAML, flash once, then revert.

---

## Monitoring Init State

Enable `debug_registers: true` and set `logger: VERBOSE` to see the decision
on every boot:

```
[lc709203f_ds] I2C device found at 0x0B, IC version: 0x0000
[lc709203f_ds] Power mode: operational
[lc709203f_ds] Read: voltage=3.882 V, RSOC=55.1 %
[lc709203f_ds] Existing gauge state considered valid; skipping Initial RSOC
```

vs. after battery swap or first boot:

```
[lc709203f_ds] Values implausible (V=0.000, RSOC=0.0%); running safe initialization
[lc709203f_ds] Writing APA 0x19...
[lc709203f_ds] Writing Initial RSOC...
[lc709203f_ds] Initialization complete
```
