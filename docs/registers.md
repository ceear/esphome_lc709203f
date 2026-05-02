# LC709203F Register Reference

All register operations use CRC-8 (polynomial `0x07`) appended to every
read and write transaction.  The I2C address is **0x0B** (7-bit).

---

## CRC-8 Protocol

| Direction | CRC input bytes                                           |
|-----------|-----------------------------------------------------------|
| Write     | `[addr_write, reg, data_low, data_high]`                  |
| Read      | `[addr_write, reg, addr_read, data_low, data_high]`       |

Where `addr_write = (0x0B << 1) = 0x16` and `addr_read = 0x17`.

The device appends its own CRC byte after the two data bytes on reads.
The host must calculate and append a CRC byte after the two data bytes on writes.

---

## Register Map

| Addr | Name                    | R/W | Format                   | Scale        | Used by this component |
|------|-------------------------|-----|--------------------------|--------------|------------------------|
| 0x06 | THERMISTOR_B            | R/W | uint16                   | raw B-const  | optional (temp mode)   |
| 0x07 | INITIAL_RSOC            | W   | magic `0xAA55`           | —            | init only              |
| 0x08 | CELL_TEMPERATURE        | R/W | uint16, 0.1 K units      | ÷10 − 273.15 | optional sensor        |
| 0x09 | CELL_VOLTAGE            | R   | uint16, mV               | ÷1000 → V    | voltage sensor         |
| 0x0A | CURRENT_DIRECTION       | R/W | 0=auto, 1=charge, 2=dis  | —            | not used               |
| 0x0B | APA                     | R/W | uint8 (lo byte)          | see table    | init / set_apa_on_boot |
| 0x0C | APT                     | R/W | uint16                   | —            | not used               |
| 0x0D | RSOC                    | R   | uint16, integer %        | direct       | debug only             |
| 0x0F | ITE                     | R   | uint16, 0.1 % units      | ÷10 → %      | level sensor           |
| 0x11 | IC_VERSION              | R   | uint16                   | —            | probe / dump           |
| 0x12 | CHANGE_OF_THE_PARAMETER | W   | uint16 profile index     | see below    | init                   |
| 0x13 | ALARM_LOW_RSOC          | R/W | uint16 %                 | —            | not used               |
| 0x14 | ALARM_LOW_CELL_VOLTAGE  | R/W | uint16 mV                | —            | not used               |
| 0x15 | IC_POWER_MODE           | R/W | `0x0001`=op, `0x0002`=sl | —            | every boot             |
| 0x16 | STATUS_BIT              | R/W | `0x0000`=I²C, `0x0001`=NTC | —          | set_temperature_mode   |
| 0x1A | NUMBER_OF_THE_PARAMETER | R   | uint16                   | —            | debug dump             |

---

### REG_APA (0x0B) – Adjustment Pack Application

Compensates for pack parasitic impedance.  Higher capacity → higher APA.

| Battery capacity (mAh) | APA value |
|------------------------|-----------|
| 100                    | 0x08      |
| 200                    | 0x0B      |
| 500                    | 0x10      |
| 1000                   | 0x19      |
| 2000                   | 0x2D      |
| 3000                   | 0x36      |

Values between table entries are linearly interpolated.
Source: ABLIC / ON Semiconductor Application Note.

---

### REG_INITIAL_RSOC (0x07)

Writing `0xAA55` to this register triggers the RSOC initialisation algorithm.
The chip measures the open-circuit voltage at that instant and uses it as a
starting point for the RSOC estimate.

**Important:** the measurement is most accurate when the battery is at rest
(load < 0.025 C).  Writing this register during heavy load produces an
inaccurate starting RSOC.

After writing, the chip needs approximately **2 ms** before RSOC register
reads become valid.

---

### REG_IC_POWER_MODE (0x15)

| Value  | Mode        | Notes                                                |
|--------|-------------|------------------------------------------------------|
| 0x0001 | Operational | Normal fuel-gauge operation; tracks charge/discharge |
| 0x0002 | Sleep       | Low power; OCV estimation paused                     |

**Deep-sleep note:** The chip has its own sleep mode that is separate from the
ESP32 deep sleep.  Keep the LC709203F in **Operational mode** so it tracks
battery drain while the ESP32 sleeps.  If the chip is inadvertently placed in
its own sleep mode, `set_operational_mode_on_boot: true` (the default) will
wake it on every ESP32 boot.

---

### REG_CHANGE_OF_THE_PARAMETER (0x12)

Selects the internal battery chemistry / voltage profile:

| Value  | Profile  | Typical use                              |
|--------|----------|------------------------------------------|
| 0x0000 | 3.8 V    | Standard LiPo (4.2 V max)  ← default    |
| 0x0001 | 3.7 V    | Older Li-Ion or low-voltage variants     |

Other profile values may exist in some chip revisions; consult the latest
ABLIC/Renesas application note for your specific chip version.

---

### REG_STATUS_BIT (0x16) – Temperature acquisition mode

| Value  | Mode        | Description                                    |
|--------|-------------|------------------------------------------------|
| 0x0000 | I²C mode    | Host writes temperature to REG_CELL_TEMPERATURE|
| 0x0001 | Thermistor  | IC measures attached NTC thermistor via REG_APT|

Default after power-on: I²C mode with an internal default of 25 °C (298.15 K).
This means temperature compensation works even without an external thermistor.

---

## Power-On-Reset Detection

**The LC709203F does not expose a dedicated POR (Power-On Reset) status bit.**

Unlike the MAX17055 (which has an explicit `POR` flag in the `STATUS` register),
the LC709203F provides no register that definitively indicates whether
initialisation is required after a power cycle.

This component uses a **plausibility check** as a substitute POR detector:

1. Read `CELL_VOLTAGE` (0x09) and `ITE` (0x0F).
2. If both are within configurable bounds (default: 2.5–4.35 V, 0–100 %),
   the chip is assumed to be running correctly and INITIAL_RSOC is **not** written.
3. If either is out of range (0x0000, 0xFFFF, or outside bounds), the chip is
   assumed to need initialisation.

This heuristic handles:
- First power-on (voltage = 0 before chip stabilises)
- Chip brownout / corrupted state (nonsense register values)
- Normal ESP32 wake-up from deep sleep (LC709203F already running → plausible values)

Documented in [`initialization.md`](initialization.md).
