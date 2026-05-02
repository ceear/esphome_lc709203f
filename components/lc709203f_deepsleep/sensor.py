import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_TEMPERATURE,
    CONF_UPDATE_INTERVAL,
    UNIT_VOLT,
    UNIT_PERCENT,
    UNIT_CELSIUS,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]

lc709203f_deepsleep_ns = cg.esphome_ns.namespace("lc709203f_deepsleep")
LC709203FDeepSleep = lc709203f_deepsleep_ns.class_(
    "LC709203FDeepSleep", cg.PollingComponent, i2c.I2CDevice
)

OperatingMode = lc709203f_deepsleep_ns.enum("OperatingMode")
OPERATING_MODE_OPTIONS = {
    "normal": OperatingMode.LC709203F_MODE_NORMAL,
    "deep_sleep": OperatingMode.LC709203F_MODE_DEEP_SLEEP,
}

# Custom config keys
CONF_BATTERY_SIZE               = "battery_size"
CONF_PACK_VOLTAGE               = "pack_voltage"
CONF_MODE                       = "mode"
CONF_ASSUME_ALREADY_INITIALIZED = "assume_already_initialized"
CONF_INITIALIZE_IF_INVALID      = "initialize_if_invalid"
CONF_FORCE_INITIALIZE           = "force_initialize"
CONF_WRITE_INITIAL_RSOC_ON_BOOT = "write_initial_rsoc_on_boot"
CONF_SET_OPERATIONAL_MODE_ON_BOOT = "set_operational_mode_on_boot"
CONF_SET_TEMPERATURE_MODE_ON_BOOT = "set_temperature_mode_on_boot"
CONF_SET_APA_ON_BOOT            = "set_apa_on_boot"
CONF_VALID_VOLTAGE_MIN          = "valid_voltage_min"
CONF_VALID_VOLTAGE_MAX          = "valid_voltage_max"
CONF_VALID_RSOC_MIN             = "valid_rsoc_min"
CONF_VALID_RSOC_MAX             = "valid_rsoc_max"
CONF_DEBUG_REGISTERS            = "debug_registers"
CONF_BATTERY_VOLTAGE            = "battery_voltage"
CONF_BATTERY_LEVEL              = "battery_level"

# Battery profile: selects the CHANGE_OF_THE_PARAMETER register value
# 3.8V profile (0x0000) suits most modern LiPo cells (nominal 3.7 V, max 4.2 V)
# 3.7V profile (0x0001) suits older Li-Ion or specific chemistries
PACK_VOLTAGE_OPTIONS = {
    "3.8V": 0x0000,
    "3.7V": 0x0001,
}


def _validate_config(config):
    mode = config.get(CONF_MODE, "deep_sleep")
    force = config.get(CONF_FORCE_INITIALIZE, False)
    assume = config.get(CONF_ASSUME_ALREADY_INITIALIZED, True)
    write_rsoc = config.get(CONF_WRITE_INITIAL_RSOC_ON_BOOT, False)

    if force and assume:
        raise cv.Invalid(
            "force_initialize: true overrides assume_already_initialized: true. "
            "Set assume_already_initialized: false when using force_initialize: true "
            "to make the intent explicit."
        )

    if write_rsoc and mode == "deep_sleep":
        raise cv.Invalid(
            "write_initial_rsoc_on_boot: true resets the RSOC algorithm on every ESP32 "
            "wake-up.  In deep_sleep mode the LC709203F keeps running between reboots, "
            "so resetting RSOC each time reduces accuracy.  "
            "Set force_initialize: true if you intentionally want a full re-init, "
            "or keep write_initial_rsoc_on_boot: false (the default for deep_sleep mode)."
        )

    v_min = config.get(CONF_VALID_VOLTAGE_MIN, 2.5)
    v_max = config.get(CONF_VALID_VOLTAGE_MAX, 4.35)
    if v_min >= v_max:
        raise cv.Invalid(
            f"valid_voltage_min ({v_min}) must be less than valid_voltage_max ({v_max})"
        )

    r_min = config.get(CONF_VALID_RSOC_MIN, 0.0)
    r_max = config.get(CONF_VALID_RSOC_MAX, 100.0)
    if r_min >= r_max:
        raise cv.Invalid(
            f"valid_rsoc_min ({r_min}) must be less than valid_rsoc_max ({r_max})"
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LC709203FDeepSleep),
            # Battery hardware parameters
            cv.Optional(CONF_BATTERY_SIZE, default=1000): cv.int_range(min=100, max=6000),
            cv.Optional(CONF_PACK_VOLTAGE, default="3.8V"): cv.enum(
                PACK_VOLTAGE_OPTIONS, upper=False
            ),
            # Operating mode
            cv.Optional(CONF_MODE, default="deep_sleep"): cv.enum(
                OPERATING_MODE_OPTIONS, lower=True
            ),
            # Initialization control
            cv.Optional(CONF_ASSUME_ALREADY_INITIALIZED, default=True): cv.boolean,
            cv.Optional(CONF_INITIALIZE_IF_INVALID, default=True): cv.boolean,
            cv.Optional(CONF_FORCE_INITIALIZE, default=False): cv.boolean,
            cv.Optional(CONF_WRITE_INITIAL_RSOC_ON_BOOT, default=False): cv.boolean,
            cv.Optional(CONF_SET_OPERATIONAL_MODE_ON_BOOT, default=True): cv.boolean,
            cv.Optional(CONF_SET_TEMPERATURE_MODE_ON_BOOT, default=False): cv.boolean,
            cv.Optional(CONF_SET_APA_ON_BOOT, default=False): cv.boolean,
            # Plausibility thresholds for the "already initialized" check
            cv.Optional(CONF_VALID_VOLTAGE_MIN, default=2.5): cv.float_range(
                min=0.0, max=5.0
            ),
            cv.Optional(CONF_VALID_VOLTAGE_MAX, default=4.35): cv.float_range(
                min=0.0, max=5.0
            ),
            cv.Optional(CONF_VALID_RSOC_MIN, default=0.0): cv.float_range(
                min=0.0, max=100.0
            ),
            cv.Optional(CONF_VALID_RSOC_MAX, default=100.0): cv.float_range(
                min=0.0, max=100.0
            ),
            # Debug
            cv.Optional(CONF_DEBUG_REGISTERS, default=False): cv.boolean,
            # Sensors (all optional)
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:battery-charging",
            ),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:thermometer",
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x0B)),
    _validate_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_battery_size(config[CONF_BATTERY_SIZE]))
    cg.add(var.set_pack_voltage(config[CONF_PACK_VOLTAGE]))
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_assume_already_initialized(config[CONF_ASSUME_ALREADY_INITIALIZED]))
    cg.add(var.set_initialize_if_invalid(config[CONF_INITIALIZE_IF_INVALID]))
    cg.add(var.set_force_initialize(config[CONF_FORCE_INITIALIZE]))
    cg.add(var.set_write_initial_rsoc_on_boot(config[CONF_WRITE_INITIAL_RSOC_ON_BOOT]))
    cg.add(var.set_set_operational_mode_on_boot(config[CONF_SET_OPERATIONAL_MODE_ON_BOOT]))
    cg.add(var.set_set_temperature_mode_on_boot(config[CONF_SET_TEMPERATURE_MODE_ON_BOOT]))
    cg.add(var.set_set_apa_on_boot(config[CONF_SET_APA_ON_BOOT]))
    cg.add(var.set_valid_voltage_min(config[CONF_VALID_VOLTAGE_MIN]))
    cg.add(var.set_valid_voltage_max(config[CONF_VALID_VOLTAGE_MAX]))
    cg.add(var.set_valid_rsoc_min(config[CONF_VALID_RSOC_MIN]))
    cg.add(var.set_valid_rsoc_max(config[CONF_VALID_RSOC_MAX]))
    cg.add(var.set_debug_registers(config[CONF_DEBUG_REGISTERS]))

    if CONF_BATTERY_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY_VOLTAGE])
        cg.add(var.set_voltage_sensor(sens))

    if CONF_BATTERY_LEVEL in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY_LEVEL])
        cg.add(var.set_level_sensor(sens))

    if CONF_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_TEMPERATURE])
        cg.add(var.set_temperature_sensor(sens))
