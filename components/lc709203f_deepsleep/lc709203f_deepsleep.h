#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include <cmath>

namespace esphome {
namespace lc709203f_deepsleep {

// ── Register map (LC709203F datasheet, ABLIC/ON Semiconductor) ──────────────
static constexpr uint8_t REG_THERMISTOR_B    = 0x06;  // Thermistor B constant (R/W)
static constexpr uint8_t REG_INITIAL_RSOC    = 0x07;  // Trigger RSOC init (W, magic 0xAA55)
static constexpr uint8_t REG_CELL_TEMPERATURE= 0x08;  // Cell temperature 0.1 K units (R/W)
static constexpr uint8_t REG_CELL_VOLTAGE    = 0x09;  // Cell voltage in mV (R)
static constexpr uint8_t REG_CURRENT_DIR     = 0x0A;  // Current direction auto/charge/discharge (R/W)
static constexpr uint8_t REG_APA             = 0x0B;  // Adjustment Pack Application (R/W)
static constexpr uint8_t REG_APT             = 0x0C;  // Adjustment Pack Thermistor (R/W)
static constexpr uint8_t REG_RSOC            = 0x0D;  // RSOC integer % (R)
static constexpr uint8_t REG_ITE             = 0x0F;  // Indicator To Empty, 0.1 % units (R)
static constexpr uint8_t REG_IC_VERSION      = 0x11;  // IC version/ID (R)
static constexpr uint8_t REG_CHANGE_PARAM    = 0x12;  // Battery profile selector (W)
static constexpr uint8_t REG_ALARM_RSOC      = 0x13;  // Low RSOC alarm threshold (R/W)
static constexpr uint8_t REG_ALARM_VOLTAGE   = 0x14;  // Low voltage alarm threshold (R/W)
static constexpr uint8_t REG_IC_POWER_MODE   = 0x15;  // Power mode control (R/W)
static constexpr uint8_t REG_STATUS_BIT      = 0x16;  // Temperature acquisition mode (R/W)
static constexpr uint8_t REG_NUM_PARAMETER   = 0x1A;  // Number of parameter (R)

// ── Power mode values ────────────────────────────────────────────────────────
static constexpr uint16_t POWER_MODE_OPERATIONAL = 0x0001;
static constexpr uint16_t POWER_MODE_SLEEP       = 0x0002;

// ── Initial RSOC trigger value ───────────────────────────────────────────────
// Writing this to REG_INITIAL_RSOC makes the chip estimate RSOC from current OCV.
// Should only be written when the battery is at rest (< 0.025C load).
static constexpr uint16_t INITIAL_RSOC_MAGIC = 0xAA55;

// ── Temperature acquisition mode ─────────────────────────────────────────────
static constexpr uint16_t TEMP_MODE_I2C        = 0x0000;  // Host writes temperature via I2C
static constexpr uint16_t TEMP_MODE_THERMISTOR = 0x0001;  // IC measures NTC thermistor

// ── CRC-8 polynomial (SMBUS/IEC 60870-5) ─────────────────────────────────────
static constexpr uint8_t CRC8_POLYNOMIAL = 0x07;

// ── Operating mode selects initialization strategy ───────────────────────────
// Unscoped enum so ESPHome codegen can reference values as lc709203f_deepsleep::LC709203F_MODE_*
enum OperatingMode : uint8_t {
  LC709203F_MODE_NORMAL     = 0,  // Always full init; suitable for always-on nodes
  LC709203F_MODE_DEEP_SLEEP = 1,  // Smart init: skip if chip already running; for deep-sleep nodes
};

// ── Records what happened during setup() ─────────────────────────────────────
enum class InitState : uint8_t {
  UNKNOWN       = 0,
  VALID_EXISTING = 1,  // Existing gauge state accepted; no INITIAL_RSOC written
  INITIALIZED   = 2,  // Full or partial init performed
  FAILED        = 3,
};

class LC709203FDeepSleep : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  // ── Config setters (called from sensor.py generated code) ─────────────────
  void set_battery_size(uint16_t v) { battery_size_ = v; }
  void set_pack_voltage(uint16_t v) { pack_voltage_ = v; }
  void set_mode(OperatingMode v) { mode_ = v; }
  void set_assume_already_initialized(bool v) { assume_already_initialized_ = v; }
  void set_initialize_if_invalid(bool v) { initialize_if_invalid_ = v; }
  void set_force_initialize(bool v) { force_initialize_ = v; }
  void set_write_initial_rsoc_on_boot(bool v) { write_initial_rsoc_on_boot_ = v; }
  void set_set_operational_mode_on_boot(bool v) { set_operational_mode_on_boot_ = v; }
  void set_set_temperature_mode_on_boot(bool v) { set_temperature_mode_on_boot_ = v; }
  void set_set_apa_on_boot(bool v) { set_apa_on_boot_ = v; }
  void set_valid_voltage_min(float v) { valid_voltage_min_ = v; }
  void set_valid_voltage_max(float v) { valid_voltage_max_ = v; }
  void set_valid_rsoc_min(float v) { valid_rsoc_min_ = v; }
  void set_valid_rsoc_max(float v) { valid_rsoc_max_ = v; }
  void set_debug_registers(bool v) { debug_registers_ = v; }

  // ── Sensor setters ─────────────────────────────────────────────────────────
  void set_voltage_sensor(sensor::Sensor *s) { voltage_sensor_ = s; }
  void set_level_sensor(sensor::Sensor *s) { level_sensor_ = s; }
  void set_temperature_sensor(sensor::Sensor *s) { temperature_sensor_ = s; }

 protected:
  // ── I2C helpers with CRC-8 ─────────────────────────────────────────────────
  bool read_reg_(uint8_t reg, uint16_t &val);
  bool write_reg_(uint8_t reg, uint16_t val);
  static uint8_t crc8_(const uint8_t *buf, size_t len);

  // ── Initialization logic ───────────────────────────────────────────────────
  bool setup_normal_();      // Full init (normal mode)
  bool setup_deep_sleep_();  // Smart init (deep-sleep mode)
  bool init_chip_();         // Writes APA, profile, INITIAL_RSOC, temp mode

  // ── Measurement ───────────────────────────────────────────────────────────
  bool read_and_publish_();

  // ── Utility ───────────────────────────────────────────────────────────────
  uint8_t apa_for_size_(uint16_t size_mah);
  void dump_registers_();

  // ── Configuration ─────────────────────────────────────────────────────────
  uint16_t battery_size_{1000};
  uint16_t pack_voltage_{0x0000};  // 0x0000 = 3.8 V profile, 0x0001 = 3.7 V profile
  OperatingMode mode_{LC709203F_MODE_DEEP_SLEEP};
  bool assume_already_initialized_{true};
  bool initialize_if_invalid_{true};
  bool force_initialize_{false};
  bool write_initial_rsoc_on_boot_{false};
  bool set_operational_mode_on_boot_{true};
  bool set_temperature_mode_on_boot_{false};
  bool set_apa_on_boot_{false};
  float valid_voltage_min_{2.5f};
  float valid_voltage_max_{4.35f};
  float valid_rsoc_min_{0.0f};
  float valid_rsoc_max_{100.0f};
  bool debug_registers_{false};

  // ── Sensors ────────────────────────────────────────────────────────────────
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *level_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};

  // ── Runtime state ─────────────────────────────────────────────────────────
  InitState init_state_{InitState::UNKNOWN};
  bool ready_{false};
  // True when the APA register didn't match the expected value on boot,
  // which reliably indicates the chip lost power (POR substitute).
  bool chip_was_reset_{false};
};

}  // namespace lc709203f_deepsleep
}  // namespace esphome
