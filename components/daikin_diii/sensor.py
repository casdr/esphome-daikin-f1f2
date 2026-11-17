import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
)

from . import CONF_DAIKIN_DIII_ID, CONF_UNIT, DaikinDIII

DEPENDENCIES = ["daikin_diii"]

# key -> index in diii::UnitField
UNIT_SENSORS = {
    "room_temperature": 0,
    "intake_temperature": 1,
    "remote_temperature": 2,
    "pipe_a_temperature": 3,
    "pipe_b_temperature": 4,
    "setpoint": 5,
}
# key -> index in diii::SystemField
SYSTEM_SENSORS = {
    "outdoor_temperature": 0,
    "evaporating_temperature": 1,
    "condensing_temperature": 2,
    "discharge_temperature_1": 3,
    "discharge_temperature_2": 4,
}
# key -> (index in diii::SystemField, unit, icon)  (plain numbers, hypotheses -> diagnostic)
SYSTEM_RAW = {
    "capacity": (5, None, "mdi:gauge"),
    "compressor_step_1": (6, None, "mdi:speedometer"),
    "compressor_step_2": (7, None, "mdi:speedometer"),
    "fan_step_1": (8, None, "mdi:fan"),
    "fan_step_2": (9, None, "mdi:fan"),
    "compressor_value_1": (10, None, "mdi:numeric"),
    "compressor_value_2": (11, None, "mdi:numeric"),
}
DIAG = {"frames_ok": 0, "frames_error": 1, "write_queue": 2, "writes_lost": 3}
# key -> index in diii::UnitField, plain numbers (hydrobox)
UNIT_RAW = {"demand_step": 6}
# key -> index in UnitDiag (per group master)
UNIT_DIAG = {"setting_commands": 0, "write_budget": 1}

TEMP_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)


RAW_SCHEMA = {k: sensor.sensor_schema(icon=icon, accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                                      entity_category=ENTITY_CATEGORY_DIAGNOSTIC) for k, (_, _, icon) in SYSTEM_RAW.items()}
DIAG_SCHEMA = {
    "frames_ok": sensor.sensor_schema(icon="mdi:counter", accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING,
                                      entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    "frames_error": sensor.sensor_schema(icon="mdi:alert-circle-outline", accuracy_decimals=0,
                                         state_class=STATE_CLASS_TOTAL_INCREASING, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    "write_queue": sensor.sensor_schema(icon="mdi:tray-full", accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                                        entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # frames this bridge gave up on: no intact echo on the bus after every retry (since boot)
    "writes_lost": sensor.sensor_schema(icon="mdi:email-remove-outline", accuracy_decimals=0,
                                        state_class=STATE_CLASS_TOTAL_INCREASING,
                                        entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}
UNIT_DIAG_SCHEMA = {
    # A5 40/41 frames to this group seen on the bus (iTM, remotes, us); HA statistics give the daily change
    "setting_commands": sensor.sensor_schema(icon="mdi:counter", accuracy_decimals=0,
                                             state_class=STATE_CLASS_TOTAL_INCREASING,
                                             entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # commands this bridge may still send to the group right now
    "write_budget": sensor.sensor_schema(icon="mdi:battery-charging-medium", accuracy_decimals=0,
                                         state_class=STATE_CLASS_MEASUREMENT,
                                         entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}


def _validate(config):
    has_unit = CONF_UNIT in config
    for key in list(UNIT_SENSORS) + list(UNIT_DIAG) + list(UNIT_RAW):
        if key in config and not has_unit:
            raise cv.Invalid(f"'{key}' requires 'unit'")
    for key in list(SYSTEM_SENSORS) + list(SYSTEM_RAW) + list(DIAG):
        if key in config and has_unit:
            raise cv.Invalid(f"'{key}' is a system sensor; remove 'unit'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
            cv.Optional(CONF_UNIT): cv.hex_uint8_t,
            **{cv.Optional(k): TEMP_SCHEMA for k in list(UNIT_SENSORS) + list(SYSTEM_SENSORS)},
            **{cv.Optional(k): v for k, v in RAW_SCHEMA.items()},
            **{cv.Optional(k): v for k, v in DIAG_SCHEMA.items()},
            **{cv.Optional(k): v for k, v in UNIT_DIAG_SCHEMA.items()},
            # hydrobox F5 22B @9: 0 without heat demand, 1-15 with (hyp: requested capacity step)
            cv.Optional("demand_step"): sensor.sensor_schema(icon="mdi:stairs-up", accuracy_decimals=0,
                                                             state_class=STATE_CLASS_MEASUREMENT),
        }
    ),
    _validate,
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    for key, idx in UNIT_SENSORS.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_unit_sensor(config[CONF_UNIT], idx, s))
    for key, idx in UNIT_RAW.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_unit_sensor(config[CONF_UNIT], idx, s))
    for key, idx in UNIT_DIAG.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_unit_diag_sensor(config[CONF_UNIT], idx, s))
    for key, idx in SYSTEM_SENSORS.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_system_sensor(idx, s))
    for key, (idx, _, _) in SYSTEM_RAW.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_system_sensor(idx, s))
    for key, idx in DIAG.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(hub.set_diag_sensor(idx, s))
