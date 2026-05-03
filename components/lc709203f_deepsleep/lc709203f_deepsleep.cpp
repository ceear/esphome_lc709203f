#include "lc709203f_deepsleep.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace lc709203f_deepsleep {

static const char *const TAG = "lc709203f_ds";

// ── APA lookup table from LC709203F Application Note ──────────────────────────
// Maps battery capacity (mAh) to the APA register value.
// Values between entries are linearly interpolated.
static const uint16_t APA_SIZE_TABLE[] = {100, 200, 500, 1000, 2000, 3000};
static const uint8_t  APA_VAL_TABLE[]  = {0x08, 0x0B, 0x10, 0x19, 0x2D, 0x36};
static const size_t   APA_TABLE_LEN    = sizeof(APA_SIZE_TABLE) / sizeof(APA_SIZE_TABLE[0]);

// ── CRC-8 (polynomial 0x07, as required by LC709203F datasheet) ───────────────
uint8_t LC709203FDeepSleep::crc8_(const uint8_t *buf, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? ((crc << 1) ^ CRC8_POLYNOMIAL) : (crc << 1);
  }
  return crc;
}

// ── I2C read with CRC verification ───────────────────────────────────────────
// The LC709203F CRC covers the address bytes as well:
//   CRC input for read: [addr_write, reg, addr_read, data_low, data_high]
bool LC709203FDeepSleep::read_reg_(uint8_t reg, uint16_t &val) {
  uint8_t raw[3];
  if (this->read_register(reg, raw, 3) != i2c::NO_ERROR)
    return false;

  const uint8_t addr_w = (uint8_t)(this->address_ << 1);
  const uint8_t addr_r = (uint8_t)((this->address_ << 1) | 0x01);
  uint8_t crc_in[5] = {addr_w, reg, addr_r, raw[0], raw[1]};
  if (crc8_(crc_in, 5) != raw[2]) {
    ESP_LOGW(TAG, "CRC mismatch reading reg 0x%02X (got 0x%02X, expected 0x%02X)",
             reg, raw[2], crc8_(crc_in, 5));
    return false;
  }

  val = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
  return true;
}

// ── I2C write with CRC ────────────────────────────────────────────────────────
// CRC input for write: [addr_write, reg, data_low, data_high]
bool LC709203FDeepSleep::write_reg_(uint8_t reg, uint16_t val) {
  const uint8_t addr_w = (uint8_t)(this->address_ << 1);
  const uint8_t dl = (uint8_t)(val & 0xFF);
  const uint8_t dh = (uint8_t)(val >> 8);
  uint8_t crc_in[4] = {addr_w, reg, dl, dh};
  uint8_t payload[3] = {dl, dh, crc8_(crc_in, 4)};
  return (this->write_register(reg, payload, 3) == i2c::NO_ERROR);
}

// ── APA interpolation ─────────────────────────────────────────────────────────
uint8_t LC709203FDeepSleep::apa_for_size_(uint16_t size_mah) {
  if (size_mah <= APA_SIZE_TABLE[0])
    return APA_VAL_TABLE[0];
  if (size_mah >= APA_SIZE_TABLE[APA_TABLE_LEN - 1])
    return APA_VAL_TABLE[APA_TABLE_LEN - 1];
  for (size_t i = 0; i < APA_TABLE_LEN - 1; i++) {
    if (size_mah <= APA_SIZE_TABLE[i + 1]) {
      float t = (float)(size_mah - APA_SIZE_TABLE[i]) /
                (float)(APA_SIZE_TABLE[i + 1] - APA_SIZE_TABLE[i]);
      return (uint8_t)(APA_VAL_TABLE[i] + t * (int)(APA_VAL_TABLE[i + 1] - APA_VAL_TABLE[i]) + 0.5f);
    }
  }
  return APA_VAL_TABLE[APA_TABLE_LEN - 1];
}

// ── Full chip initialization ──────────────────────────────────────────────────
// Writes the APA, battery profile, INITIAL_RSOC trigger, and optionally the
// temperature mode.  This is the sequence needed when the chip has just powered
// on or when previously read values are implausible.
bool LC709203FDeepSleep::init_chip_() {
  const uint8_t apa = apa_for_size_(battery_size_);

  ESP_LOGI(TAG, "Writing APA 0x%02X for %u mAh battery...", apa, battery_size_);
  if (!write_reg_(REG_APA, apa)) {
    ESP_LOGE(TAG, "Failed to write APA register");
    return false;
  }
  ESP_LOGD(TAG, "  APA write OK (reg 0x%02X = 0x%02X)", REG_APA, apa);

  ESP_LOGD(TAG, "Writing battery profile (CHANGE_OF_THE_PARAMETER = 0x%04X)...", pack_voltage_);
  if (!write_reg_(REG_CHANGE_PARAM, pack_voltage_)) {
    ESP_LOGW(TAG, "Failed to write battery profile; continuing anyway");
  } else {
    ESP_LOGD(TAG, "  CHANGE_PARAM write OK");
  }

  ESP_LOGI(TAG, "Writing Initial RSOC...");
  if (!write_reg_(REG_INITIAL_RSOC, INITIAL_RSOC_MAGIC)) {
    ESP_LOGE(TAG, "Failed to write Initial RSOC");
    return false;
  }
  ESP_LOGD(TAG, "  INITIAL_RSOC write OK – chip re-estimates RSOC from current OCV (battery should be at rest)");
  // Datasheet recommends a brief settle time after INITIAL_RSOC write
  delay(2);

  if (set_temperature_mode_on_boot_) {
    uint16_t temp_mode = (temperature_sensor_ != nullptr) ? TEMP_MODE_THERMISTOR : TEMP_MODE_I2C;
    ESP_LOGD(TAG, "Writing temperature mode 0x%04X (%s)...", temp_mode,
             temp_mode == TEMP_MODE_THERMISTOR ? "thermistor" : "I2C");
    write_reg_(REG_STATUS_BIT, temp_mode);
  } else {
    ESP_LOGD(TAG, "  STATUS_BIT (temp mode) not written (set_temperature_mode_on_boot=false)");
  }

  ESP_LOGI(TAG, "Initialization complete");
  return true;
}

// ── Normal mode setup ─────────────────────────────────────────────────────────
// Mirrors the ESPHome standard component but completes all init in setup()
// rather than spreading it across multiple update() cycles.
bool LC709203FDeepSleep::setup_normal_() {
  ESP_LOGD(TAG, "Normal mode: full initialization sequence");
  ESP_LOGD(TAG, "  Will write: IC_POWER_MODE=operational, APA=0x%02X (%u mAh), CHANGE_PARAM=0x%04X, INITIAL_RSOC",
           apa_for_size_(battery_size_), battery_size_, pack_voltage_);

  if (!write_reg_(REG_IC_POWER_MODE, POWER_MODE_OPERATIONAL)) {
    ESP_LOGE(TAG, "Failed to set operational mode");
    return false;
  }
  ESP_LOGD(TAG, "  IC_POWER_MODE=operational write OK");
  return init_chip_();
}

// ── Deep-sleep mode setup ─────────────────────────────────────────────────────
// Strategy: read first, initialize only when necessary.
bool LC709203FDeepSleep::setup_deep_sleep_() {
  // ── Step 1: Read current power mode ────────────────────────────────────────
  uint16_t power_mode = 0xFFFF;
  bool pm_ok = read_reg_(REG_IC_POWER_MODE, power_mode);

  if (pm_ok) {
    if (power_mode == POWER_MODE_OPERATIONAL) {
      ESP_LOGI(TAG, "Power mode: operational");
    } else if (power_mode == POWER_MODE_SLEEP) {
      ESP_LOGI(TAG, "Power mode: sleep");
    } else {
      ESP_LOGW(TAG, "Power mode: unknown (0x%04X)", power_mode);
    }
  } else {
    ESP_LOGW(TAG, "Could not read power mode register");
  }

  // ── Step 2: Wake from sleep if needed ──────────────────────────────────────
  if (set_operational_mode_on_boot_) {
    if (!pm_ok || power_mode != POWER_MODE_OPERATIONAL) {
      ESP_LOGI(TAG, "Setting operational mode...");
      if (!write_reg_(REG_IC_POWER_MODE, POWER_MODE_OPERATIONAL)) {
        ESP_LOGW(TAG, "Failed to set operational mode; measurements may be stale");
      } else {
        ESP_LOGD(TAG, "  IC_POWER_MODE=operational write OK");
      }
      delay(5);  // Brief settle after mode switch
    } else {
      ESP_LOGD(TAG, "IC_POWER_MODE already operational – no write needed");
    }
  } else {
    ESP_LOGD(TAG, "set_operational_mode_on_boot=false – IC_POWER_MODE not written");
  }

  // ── Step 3: APA register check as primary POR indicator ───────────────────
  // After chip power loss the APA register resets to 0x0000.
  // We always write a non-zero APA during init, so a mismatch means the chip
  // lost power since the last initialisation – more reliable than voltage/RSOC
  // range checks, which can coincidentally pass right after a fresh power-on.
  {
    const uint8_t expected_apa = apa_for_size_(battery_size_);
    uint16_t raw_apa = 0;
    if (!read_reg_(REG_APA, raw_apa)) {
      chip_was_reset_ = true;
      ESP_LOGD(TAG, "APA register unreadable → treating as chip reset");
    } else {
      const uint8_t chip_apa = (uint8_t)(raw_apa & 0xFF);
      if (chip_apa != expected_apa) {
        chip_was_reset_ = true;
        ESP_LOGW(TAG, "APA mismatch: chip=0x%02X expected=0x%02X → chip lost power, initialization required",
                 chip_apa, expected_apa);
      } else {
        chip_was_reset_ = false;
        ESP_LOGD(TAG, "APA match: chip=0x%02X expected=0x%02X → chip retained state across sleep",
                 chip_apa, expected_apa);
      }
    }
  }

  // ── Step 4: force_initialize overrides everything ──────────────────────────
  if (force_initialize_) {
    chip_was_reset_ = true;  // Treat forced init the same as hardware reset for logging
    ESP_LOGI(TAG, "force_initialize=true: running full initialization");
    ESP_LOGD(TAG, "  Reason: unconditional re-init (e.g. battery swap, first commissioning)");
    ESP_LOGD(TAG, "  Will write: IC_POWER_MODE, APA=0x%02X (%u mAh), CHANGE_PARAM=0x%04X, INITIAL_RSOC",
             apa_for_size_(battery_size_), battery_size_, pack_voltage_);
    if (!write_reg_(REG_IC_POWER_MODE, POWER_MODE_OPERATIONAL))
      return false;
    ESP_LOGD(TAG, "  IC_POWER_MODE=operational write OK");
    if (!init_chip_())
      return false;
    init_state_ = InitState::INITIALIZED;
    ESP_LOGD(TAG, "Setup complete: init_state=initialized (forced)");
    return true;
  }

  // ── Step 5: Try reading existing values (only when APA matched) ────────────
  // chip_was_reset_=true means APA check already determined init is needed –
  // skip the voltage/RSOC check entirely to avoid false-positives.
  if (!chip_was_reset_ && assume_already_initialized_) {
    ESP_LOGD(TAG, "APA matched – reading voltage/RSOC as secondary sanity check");
    uint16_t raw_v = 0, raw_l = 0;
    bool read_ok = read_reg_(REG_CELL_VOLTAGE, raw_v) && read_reg_(REG_ITE, raw_l);

    if (read_ok) {
      float voltage = raw_v / 1000.0f;
      float level   = raw_l / 10.0f;

      bool v_ok = (voltage >= valid_voltage_min_ && voltage <= valid_voltage_max_);
      bool l_ok = (level   >= valid_rsoc_min_    && level   <= valid_rsoc_max_);

      ESP_LOGD(TAG, "Plausibility check:");
      ESP_LOGD(TAG, "  Voltage: %.3f V  [%.2f–%.2f V] → %s",
               voltage, valid_voltage_min_, valid_voltage_max_, v_ok ? "OK" : "OUT OF RANGE");
      ESP_LOGD(TAG, "  RSOC:    %.1f %%  [%.0f–%.0f %%] → %s",
               level, valid_rsoc_min_, valid_rsoc_max_, l_ok ? "OK" : "OUT OF RANGE");

      if (v_ok && l_ok) {
        ESP_LOGI(TAG, "Existing gauge state considered valid; skipping Initial RSOC");
        ESP_LOGD(TAG, "  Decision: chip running with valid values → no registers written");
        ESP_LOGD(TAG, "  Skipped: APA, CHANGE_PARAM, INITIAL_RSOC");
        init_state_ = InitState::VALID_EXISTING;
        ESP_LOGD(TAG, "Setup complete: init_state=valid_existing");
        return true;
      }

      ESP_LOGW(TAG, "Values implausible (V=%.3f, RSOC=%.1f%%); running safe initialization",
               voltage, level);
      ESP_LOGD(TAG, "  Decision: APA matched but values out of range → initialization required");
    } else {
      ESP_LOGW(TAG, "Failed to read voltage/RSOC; running safe initialization");
      ESP_LOGD(TAG, "  Decision: I2C read error → initialization required");
    }
  } else {
    ESP_LOGD(TAG, "assume_already_initialized=false – skipping plausibility check, proceeding to init");
  }

  // ── Step 5b: Full init when chip lost power (APA reset detected) ───────────
  // chip_was_reset_=true means the APA register was 0x0000 on boot, which is the
  // power-on reset value.  Always run a full init so that APA is written and the
  // next boot's APA check passes.  set_apa_on_boot / write_initial_rsoc_on_boot
  // are intentionally ignored here – those flags only affect the soft-init path
  // (APA matched but values implausible) to avoid disturbing an already-running chip.
  if (chip_was_reset_) {
    ESP_LOGI(TAG, "Chip power loss confirmed (APA reset to 0x00); running full initialization");
    if (!init_chip_()) return false;
    init_state_ = InitState::INITIALIZED;
    ESP_LOGD(TAG, "Setup complete: init_state=initialized (chip power loss)");
    return true;
  }

  // ── Step 6: Soft init when APA matched but values were implausible ─────────
  // Reaches here only when the chip retained power but voltage/RSOC was out of
  // the configured plausibility window.  Respects set_apa_on_boot and
  // write_initial_rsoc_on_boot so transient read errors don't unnecessarily reset
  // accumulated gauge state.
  if (initialize_if_invalid_) {
    ESP_LOGD(TAG, "Soft initialization (APA matched, values implausible):");
    if (set_apa_on_boot_) {
      uint8_t apa = apa_for_size_(battery_size_);
      ESP_LOGD(TAG, "  Writing APA 0x%02X (%u mAh)...", apa, battery_size_);
      write_reg_(REG_APA, apa);
    } else {
      ESP_LOGD(TAG, "  APA write skipped (set_apa_on_boot=false)");
    }

    ESP_LOGD(TAG, "  Writing CHANGE_PARAM 0x%04X...", pack_voltage_);
    write_reg_(REG_CHANGE_PARAM, pack_voltage_);

    if (write_initial_rsoc_on_boot_) {
      ESP_LOGI(TAG, "Writing Initial RSOC (write_initial_rsoc_on_boot=true)...");
      if (!write_reg_(REG_INITIAL_RSOC, INITIAL_RSOC_MAGIC)) {
        ESP_LOGE(TAG, "Failed to write Initial RSOC");
        return false;
      }
      ESP_LOGD(TAG, "  INITIAL_RSOC write OK – chip re-estimates RSOC from current OCV");
      delay(2);
    } else {
      ESP_LOGD(TAG, "  INITIAL_RSOC write skipped (write_initial_rsoc_on_boot=false) – gauge state preserved");
    }

    if (set_temperature_mode_on_boot_) {
      write_reg_(REG_STATUS_BIT, (temperature_sensor_ != nullptr) ? TEMP_MODE_THERMISTOR : TEMP_MODE_I2C);
    } else {
      ESP_LOGD(TAG, "  STATUS_BIT (temp mode) not written (set_temperature_mode_on_boot=false)");
    }

    init_state_ = InitState::INITIALIZED;
    ESP_LOGI(TAG, "Initialization complete");
    ESP_LOGD(TAG, "Setup complete: init_state=initialized (soft init, values were implausible)");
    return true;
  }

  ESP_LOGE(TAG, "Values invalid and initialize_if_invalid=false; component will not publish");
  init_state_ = InitState::FAILED;
  return false;
}

// ── Read sensors and publish states ──────────────────────────────────────────
bool LC709203FDeepSleep::read_and_publish_() {
  uint16_t raw_v = 0, raw_l = 0;

  if (!read_reg_(REG_CELL_VOLTAGE, raw_v)) {
    ESP_LOGW(TAG, "Failed to read cell voltage");
    return false;
  }
  if (!read_reg_(REG_ITE, raw_l)) {
    ESP_LOGW(TAG, "Failed to read battery level (ITE)");
    return false;
  }

  const float voltage = raw_v / 1000.0f;
  const float level   = raw_l / 10.0f;

  ESP_LOGI(TAG, "Voltage read: %.3f V", voltage);
  ESP_LOGI(TAG, "RSOC read: %.1f %%", level);

  if (voltage_sensor_ != nullptr)
    voltage_sensor_->publish_state(voltage);
  if (level_sensor_ != nullptr)
    level_sensor_->publish_state(level);

  if (temperature_sensor_ != nullptr) {
    uint16_t raw_t = 0;
    if (read_reg_(REG_CELL_TEMPERATURE, raw_t)) {
      // Register is in 0.1 K; convert to °C
      const float temp_c = raw_t / 10.0f - 273.15f;
      temperature_sensor_->publish_state(temp_c);
    } else {
      ESP_LOGW(TAG, "Failed to read cell temperature");
    }
  }

  ESP_LOGI(TAG, "Published battery values");
  return true;
}

// ── setup() ──────────────────────────────────────────────────────────────────
void LC709203FDeepSleep::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LC709203F Deep Sleep component...");

  // Probe: read IC version register to confirm the device is present and CRC works
  uint16_t ic_version = 0;
  if (!read_reg_(REG_IC_VERSION, ic_version)) {
    ESP_LOGE(TAG, "I2C device not responding at 0x%02X (no ACK or CRC error)", this->address_);
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "I2C device found at 0x%02X, IC version: 0x%04X", this->address_, ic_version);

  if (debug_registers_)
    dump_registers_();

  bool ok = false;
  if (mode_ == LC709203F_MODE_NORMAL) {
    ok = setup_normal_();
  } else {
    ok = setup_deep_sleep_();
  }

  if (ok) {
    ready_ = true;
  } else {
    // Non-fatal: update() will retry
    ESP_LOGW(TAG, "Setup did not complete cleanly; will retry in update()");
  }
}

// ── update() ─────────────────────────────────────────────────────────────────
// In deep-sleep mode with update_interval: never, this is called only when
// component.update is triggered explicitly (e.g. via on_boot automation).
// The first call after boot reads fresh values and publishes immediately.
void LC709203FDeepSleep::update() {
  // Always-visible one-liner so the boot decision is readable over WiFi/MQTT
  // even though setup() ran before the connection was established.
  if (chip_was_reset_ && init_state_ == InitState::INITIALIZED) {
    ESP_LOGI(TAG, "Boot: power loss detected → chip re-initialized (APA+RSOC written)");
  } else if (!chip_was_reset_ && init_state_ == InitState::VALID_EXISTING) {
    ESP_LOGI(TAG, "Boot: chip retained state → no registers written");
  } else if (!chip_was_reset_ && init_state_ == InitState::INITIALIZED) {
    ESP_LOGI(TAG, "Boot: values implausible → soft re-initialized (no RSOC reset)");
  } else if (init_state_ == InitState::FAILED) {
    ESP_LOGW(TAG, "Boot: initialization failed");
  } else {
    ESP_LOGW(TAG, "Boot: unknown state (chip_reset=%s init=%d)", chip_was_reset_ ? "YES" : "no", (int)init_state_);
  }

  if (debug_registers_) {
    ESP_LOGD(TAG, "  chip_reset=%s  init_state=%s",
      chip_was_reset_ ? "YES" : "no",
      init_state_ == InitState::VALID_EXISTING ? "valid_existing" :
      init_state_ == InitState::INITIALIZED    ? "initialized" :
      init_state_ == InitState::FAILED         ? "failed" : "unknown");

    const uint8_t exp_apa = apa_for_size_(battery_size_);
    uint16_t r_apa = 0, r_pm = 0, r_sb = 0, r_ver = 0;
    bool a_ok = read_reg_(REG_APA,           r_apa);
    bool p_ok = read_reg_(REG_IC_POWER_MODE, r_pm);
    bool s_ok = read_reg_(REG_STATUS_BIT,    r_sb);
    bool v_ok = read_reg_(REG_IC_VERSION,    r_ver);
    ESP_LOGD(TAG, "Chip registers:");
    ESP_LOGD(TAG, "  APA          = 0x%02X  (expected 0x%02X for %u mAh) → %s",
             a_ok ? (uint8_t)(r_apa & 0xFF) : 0xFF, exp_apa, battery_size_,
             a_ok ? ((uint8_t)(r_apa & 0xFF) == exp_apa ? "OK" : "MISMATCH") : "READ ERROR");
    ESP_LOGD(TAG, "  IC_POWER_MODE= 0x%04X  (%s)",
             p_ok ? r_pm : 0xFFFF,
             p_ok ? (r_pm == POWER_MODE_OPERATIONAL ? "operational" :
                     r_pm == POWER_MODE_SLEEP        ? "sleep" : "unknown") : "READ ERROR");
    ESP_LOGD(TAG, "  STATUS_BIT   = 0x%04X  (temp mode: %s)",
             s_ok ? r_sb : 0xFFFF,
             s_ok ? (r_sb == TEMP_MODE_THERMISTOR ? "thermistor" : "I2C") : "READ ERROR");
    ESP_LOGD(TAG, "  IC_VERSION   = 0x%04X%s", v_ok ? r_ver : 0xFFFF, v_ok ? "" : "  (READ ERROR)");
  }

  if (!ready_) {
    ESP_LOGW(TAG, "Not ready; attempting recovery initialization");
    if (!write_reg_(REG_IC_POWER_MODE, POWER_MODE_OPERATIONAL) || !init_chip_()) {
      ESP_LOGE(TAG, "Recovery failed; skipping update");
      return;
    }
    ready_ = true;
    init_state_ = InitState::INITIALIZED;
  }

  read_and_publish_();
}

// ── dump_config() ─────────────────────────────────────────────────────────────
void LC709203FDeepSleep::dump_config() {
  ESP_LOGCONFIG(TAG, "LC709203F Deep Sleep:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_ == LC709203F_MODE_DEEP_SLEEP ? "deep_sleep" : "normal");
  ESP_LOGCONFIG(TAG, "  Battery size: %u mAh  APA: 0x%02X", battery_size_, apa_for_size_(battery_size_));
  ESP_LOGCONFIG(TAG, "  Battery profile (CHANGE_PARAM): 0x%04X", pack_voltage_);
  ESP_LOGCONFIG(TAG, "  assume_already_initialized: %s", assume_already_initialized_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  initialize_if_invalid: %s", initialize_if_invalid_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  force_initialize: %s", force_initialize_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  write_initial_rsoc_on_boot: %s", write_initial_rsoc_on_boot_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  set_operational_mode_on_boot: %s", set_operational_mode_on_boot_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  set_apa_on_boot: %s", set_apa_on_boot_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Valid voltage range: %.2f – %.2f V", valid_voltage_min_, valid_voltage_max_);
  ESP_LOGCONFIG(TAG, "  Valid RSOC range: %.0f – %.0f %%", valid_rsoc_min_, valid_rsoc_max_);
  ESP_LOGCONFIG(TAG, "  Init state: %s",
    init_state_ == InitState::VALID_EXISTING ? "valid_existing" :
    init_state_ == InitState::INITIALIZED    ? "initialized" :
    init_state_ == InitState::FAILED         ? "failed" : "unknown");
  LOG_SENSOR("  ", "Battery Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "Battery Level",   this->level_sensor_);
  LOG_SENSOR("  ", "Temperature",     this->temperature_sensor_);
  LOG_I2C_DEVICE(this);
}

// ── Debug register dump ───────────────────────────────────────────────────────
void LC709203FDeepSleep::dump_registers_() {
  static const uint8_t regs[]  = {REG_THERMISTOR_B, REG_CELL_TEMPERATURE, REG_CELL_VOLTAGE,
                                    REG_APA, REG_APT, REG_RSOC, REG_ITE,
                                    REG_IC_VERSION, REG_CHANGE_PARAM, REG_ALARM_RSOC,
                                    REG_ALARM_VOLTAGE, REG_IC_POWER_MODE, REG_STATUS_BIT};
  static const char *names[]   = {"THERMISTOR_B", "CELL_TEMP", "CELL_VOLTAGE",
                                    "APA", "APT", "RSOC", "ITE",
                                    "IC_VERSION", "CHANGE_PARAM", "ALARM_RSOC",
                                    "ALARM_VOLTAGE", "IC_POWER_MODE", "STATUS_BIT"};
  const size_t N = sizeof(regs) / sizeof(regs[0]);

  ESP_LOGD(TAG, "=== Register Dump ===");
  for (size_t i = 0; i < N; i++) {
    uint16_t v = 0;
    if (read_reg_(regs[i], v)) {
      ESP_LOGD(TAG, "  [0x%02X] %-15s = 0x%04X (%5u)", regs[i], names[i], v, v);
    } else {
      ESP_LOGD(TAG, "  [0x%02X] %-15s = READ ERROR", regs[i], names[i]);
    }
  }
  ESP_LOGD(TAG, "=== End Register Dump ===");
}

}  // namespace lc709203f_deepsleep
}  // namespace esphome
