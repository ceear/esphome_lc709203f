# LC709203F Initialization Strategy

## The Core Problem

The LC709203F lacks a Power-On Reset (POR) status register.  There is no
single bit we can read to learn "has this chip been initialised yet?"

On an always-on node this does not matter – you always run full init.
On a deep-sleep node it matters a great deal:

- The LC709203F sits directly on the LiPo and runs continuously.
- The ESP32 wakes up every few minutes, runs for a few seconds, then sleeps again.
- If the ESP32 blindly runs a full init sequence (including writing `INITIAL_RSOC`)
  on every wake-up, the RSOC algorithm is reset each time, discarding the
  accumulated charge-tracking data.

---

## Mode: `normal`

Full initialisation on every ESP32 boot.  This matches the behaviour of the
official ESPHome `lc709203f` component.

**Sequence in `setup()`:**

1. Write `IC_POWER_MODE` = `0x0001` (operational)
2. Write `APA` (calculated from `battery_size`)
3. Write `CHANGE_OF_THE_PARAMETER` (battery profile)
4. Write `INITIAL_RSOC` = `0xAA55`  ← resets RSOC every boot
5. 2 ms settle time
6. Optionally write `STATUS_BIT` (temperature mode)
7. `ready = true`

First `update()` call reads `CELL_VOLTAGE` and `ITE` and publishes immediately.

---

## Mode: `deep_sleep` (default)

Smart initialisation: **read first, initialise only when necessary.**

### Decision flowchart

```
boot
 │
 ├─ force_initialize: true ──────────────────────► full init → ready
 │
 ├─ read IC_POWER_MODE
 │   ├─ SLEEP → write OPERATIONAL (if set_operational_mode_on_boot: true)
 │   └─ OPERATIONAL → continue
 │
 ├─ assume_already_initialized: true
 │   ├─ read CELL_VOLTAGE (0x09) + ITE (0x0F)
 │   │   ├─ read OK AND voltage ∈ [vmin, vmax] AND rsoc ∈ [rmin, rmax]
 │   │   │   └─ "Existing gauge state considered valid; skipping Initial RSOC"
 │   │   │       ready = true  ← NO INITIAL_RSOC WRITTEN
 │   │   │
 │   │   └─ read failed OR values out of range
 │   │       └─ fall through to init
 │   │
 │   └─ assume_already_initialized: false → fall through to init
 │
 └─ initialize_if_invalid: true
     ├─ write APA (if set_apa_on_boot: true)
     ├─ write CHANGE_OF_THE_PARAMETER
     ├─ write INITIAL_RSOC (if write_initial_rsoc_on_boot: true, else skip)
     └─ ready = true

     initialize_if_invalid: false → component not ready, no publish
```

### Log output – valid existing state (normal deep-sleep wake-up)

```
[lc709203f_ds] I2C device found at 0x0B, IC version: 0x0000
[lc709203f_ds] Power mode: operational
[lc709203f_ds] Read: voltage=3.882 V, RSOC=55.1 %
[lc709203f_ds] Existing gauge state considered valid; skipping Initial RSOC
[lc709203f_ds] Voltage read: 3.882 V
[lc709203f_ds] RSOC read: 55.1 %
[lc709203f_ds] Published battery values
```

### Log output – invalid values (first boot or after chip power loss)

```
[lc709203f_ds] I2C device found at 0x0B, IC version: 0x0000
[lc709203f_ds] Power mode: operational
[lc709203f_ds] Values implausible (V=0.000, RSOC=0.0%); running safe initialization
[lc709203f_ds] Writing APA 0x19...
[lc709203f_ds] Writing Initial RSOC...
[lc709203f_ds] Initialization complete
[lc709203f_ds] Voltage read: 3.871 V
[lc709203f_ds] RSOC read: 53.7 %
[lc709203f_ds] Published battery values
```

---

## Configuration Options Explained

### `force_initialize: false` ← default

Disables the plausibility check.  Always runs full init including `INITIAL_RSOC`.
Use only for first commissioning or when you suspect chip state corruption.

### `assume_already_initialized: true` ← default in deep_sleep mode

Attempt to read existing values before deciding whether to initialise.
If values are plausible, trust them.

### `initialize_if_invalid: true` ← default

If the plausibility check fails, run a targeted init automatically.
Set to `false` if you want the component to fail loudly rather than silently
reinitialise (e.g. for debugging chip state issues).

### `write_initial_rsoc_on_boot: false` ← default in deep_sleep mode

Controls whether `INITIAL_RSOC = 0xAA55` is written during the "invalid,
reinitialising" code path.  Even in the fallback init path, you can choose
to NOT write INITIAL_RSOC and let the chip recalibrate gradually.

Set to `true` only if you are certain the battery is at rest and you want
an immediate RSOC reset.  **Never set this together with deep_sleep mode
unless you understand that RSOC accuracy will degrade with each reboot.**

### `set_apa_on_boot: false` ← default in deep_sleep mode

APA is set once at first commissioning and retained in the chip.  Only needs
to be rewritten if `battery_size` changes or the chip loses power.

---

## Comparison with MAX17055

| Feature                        | MAX17055                        | LC709203F                           |
|--------------------------------|---------------------------------|-------------------------------------|
| POR detection                  | STATUS.POR bit                  | **None** – must use plausibility    |
| Algorithm type                 | ModelGauge m5 (Coulomb counter + OCV) | OCV lookup table + temperature |
| True Coulomb counter           | Yes                             | No – voltage-based estimation only  |
| Accuracy over deep sleep       | High (tracks current)           | Moderate (OCV-based; no current measurement) |
| Init required after POR        | Yes, load learned parameters    | Yes, write APA + INITIAL_RSOC       |
| INITIAL_RSOC equivalent        | FULLCAPREP / dQacc writes       | Write `0xAA55` to reg 0x07          |

The LC709203F is **not** a Coulomb counter – it estimates RSOC from
open-circuit voltage and temperature, not from integrated current.  This means:

- It works well when the battery is at rest (during ESP32 deep sleep).
- It is less accurate under sustained load.
- Writing `INITIAL_RSOC` on every boot is harmful because it discards the
  chip's accumulated OCV-drift calibration.
