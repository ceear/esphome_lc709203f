# Problem: Official ESPHome LC709203F Component and Deep Sleep

## Observed Behaviour

When using the standard ESPHome `lc709203f` component with an ESP32 in deep sleep:

1. The ESP32 wakes up and ESPHome runs `setup()` on all components.
2. The `lc709203f` component writes three initialisation registers:
   - `IC_POWER_MODE (0x15)` = `0x0001`
   - `APA (0x0B)` = capacity-derived value
   - `CHANGE_OF_THE_PARAMETER (0x12)` = battery profile
3. The component enters `STATE_RSOC` – but does **not yet read any sensor data**.
4. **First `component.update` call:** writes `INITIAL_RSOC (0x07)` = `0xAA55`.
   No sensor values are published.
5. **Second `component.update` call:** writes `STATUS_BIT (0x16)` for temperature mode.
   Still no sensor values.
6. **Third `component.update` call (and onwards):** reads `CELL_VOLTAGE` and `ITE`.
   Sensor values are finally published.

In the ESPHome serial log this looks like:
```
[I2C] 0x0B TX 07:55:AA:17   ← INITIAL_RSOC write (update #1)
[I2C] 0x0B TX 16:00:00:CC   ← STATUS_BIT write (update #2)
[I2C] 0x0B RX 09:...        ← CELL_VOLTAGE read (update #3)
[I2C] 0x0B RX 0F:...        ← ITE read (update #3)
```

## Why This Is a Problem for Deep-Sleep Nodes

### 1. Multiple dummy updates required

In a deep-sleep workflow, you typically call `component.update` once, get the
value, and immediately enter deep sleep.  With the standard component you must
call `component.update` **three times** before getting real data.  This means:

- Three update intervals must elapse before a reading is available.
- With `update_interval: 60s` the first reading arrives after 180 seconds –
  the entire point of fast deep-sleep measurement is defeated.
- A workaround of calling `component.update` three times in quick succession
  works but is fragile and difficult to maintain.

### 2. INITIAL_RSOC is written on every ESP32 boot

`INITIAL_RSOC` triggers the chip's RSOC estimation algorithm to restart using
the battery's current open-circuit voltage (OCV).  This is appropriate at
true power-on, but harmful on repeated ESP32 deep-sleep wake-ups because:

- The LC709203F stays powered from the LiPo at all times.
- It has been tracking voltage changes the entire time the ESP32 slept.
- Writing `INITIAL_RSOC` discards that accumulated state and forces a new
  OCV sample – which may not be accurate if the battery is not at rest.
- Over many wake/sleep cycles the RSOC reading drifts rather than converging.

### 3. No POR awareness

The standard component performs the same full initialisation sequence
regardless of whether the chip has just powered on or has been running
undisturbed for hours.  It cannot distinguish:

- "This chip just powered on and needs init" (correct: write INITIAL_RSOC)
- "This chip has been running fine for 48 hours; ESP32 just woke up"
  (wrong: writing INITIAL_RSOC degrades accuracy)

## The Fix

This external component (`lc709203f_deepsleep`) resolves these issues by:

1. **Reading before writing:** On every wake-up the component reads the
   existing voltage and RSOC values first.
2. **Plausibility check substitutes POR detection:** If values are within
   sensible bounds, the gauge is assumed to be running correctly.
3. **No INITIAL_RSOC on plausible state:** The reset command is only sent
   when values are genuinely invalid (or `force_initialize: true`).
4. **All init in `setup()`:** No state machine spanning multiple `update()`
   cycles.  The first `component.update` call after boot returns real data.
