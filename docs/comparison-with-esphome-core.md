# Comparison: This Component vs. Official ESPHome `lc709203f`

## Official ESPHome Component State Machine

Source: `esphome/components/lc709203f/lc709203f.cpp`  
URL: https://github.com/esphome/esphome/tree/dev/esphome/components/lc709203f

### `setup()` writes

| Register             | Value     | Purpose                         |
|----------------------|-----------|---------------------------------|
| IC_POWER_MODE (0x15) | 0x0001    | Wake chip to operational mode   |
| APA (0x0B)           | calculated| Battery capacity compensation   |
| CHANGE_PARAM (0x12)  | 0x0000/01 | Battery voltage profile         |

On success: `state_ = STATE_RSOC`  
On I2C failure: returns without setting state, retries in `update()`

### `update()` – STATE_RSOC (first call)

| Register          | Value    | Purpose                  |
|-------------------|----------|--------------------------|
| INITIAL_RSOC (0x07) | 0xAA55 | Reset RSOC from current OCV |

Advances to `STATE_TEMP_SETUP`

### `update()` – STATE_TEMP_SETUP (second call)

| Register           | Value    | Purpose                       |
|--------------------|----------|-------------------------------|
| STATUS_BIT (0x16)  | 0x0000 or 0x0001 | Temperature acquisition mode |
| THERMISTOR_B (0x06)| B-const  | NTC B-constant (if thermistor)|

Advances to `STATE_NORMAL`

### `update()` – STATE_NORMAL (third call and onwards)

Reads:
- `CELL_VOLTAGE (0x09)` → publish voltage
- `ITE (0x0F)` → publish battery level %
- `CELL_TEMPERATURE (0x08)` → publish temperature (if configured)

---

## This Component (`lc709203f_deepsleep`)

### `setup()` – `mode: deep_sleep`

| Step | Action                                     | Condition                         |
|------|--------------------------------------------|-----------------------------------|
| 1    | Read IC_VERSION to verify device presence  | always                            |
| 2    | Read IC_POWER_MODE                         | always                            |
| 3    | Write IC_POWER_MODE = OPERATIONAL          | if not already operational        |
| 4    | Read CELL_VOLTAGE + ITE                    | if assume_already_initialized     |
| 5    | Check plausibility                         | if read succeeded                 |
| 6    | `ready = true`, skip init                  | if values plausible               |
| 7    | Write APA, CHANGE_PARAM                    | if values implausible + init needed |
| 8    | Write INITIAL_RSOC (if configured)         | if write_initial_rsoc_on_boot     |
| 9    | `ready = true`                             | after any successful init path    |

### `update()` – all modes

No state machine.  Directly reads:
- `CELL_VOLTAGE (0x09)`
- `ITE (0x0F)`
- `CELL_TEMPERATURE (0x08)` (if sensor configured)

Publishes all values in a single call.

---

## Summary Table

| Feature                              | Official ESPHome | This component (deep_sleep) |
|--------------------------------------|------------------|-----------------------------|
| Updates needed before first reading  | 3                | **1**                       |
| INITIAL_RSOC written every boot      | Yes              | **No** (only when invalid)  |
| Full init on every boot              | Yes              | Only when necessary          |
| POR detection                        | No               | Plausibility check           |
| Configurable init strategy           | No               | Yes                          |
| Suitable for deep-sleep nodes        | No (workarounds) | **Yes (native support)**     |
| Always-on nodes                      | Yes              | Yes (`mode: normal`)         |
| Debug register dump                  | No               | `debug_registers: true`      |

---

## When to Use Which Component

**Use the official ESPHome `lc709203f`:**
- Always-on nodes where the ESP32 and LC709203F power cycle together
- When you need the official ESPHome temperature-sensor thermistor support
- When you prefer a component maintained in the ESPHome core

**Use `lc709203f_deepsleep` (this component):**
- ESP32 in deep sleep with LC709203F permanently on the battery
- Single `component.update` call must deliver immediate readings
- You want to avoid RSOC resets across deep-sleep cycles
- You need fine-grained control over which register writes happen on each boot
