import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_DAIKIN_DIII_ID, CONF_UNIT, DaikinDIII

DEPENDENCIES = ["daikin_diii"]

UNIT_TEXT = {"mode": 0, "fan": 1}
SYSTEM_TEXT = {"clock": 0, "last_write": 1}


def _validate(config):
    if any(k in config for k in UNIT_TEXT) and CONF_UNIT not in config:
        raise cv.Invalid("unit text sensors require 'unit'")
    if any(k in config for k in SYSTEM_TEXT) and CONF_UNIT in config:
        raise cv.Invalid("system text sensors must not have 'unit'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
            cv.Optional(CONF_UNIT): cv.hex_uint8_t,
            **{cv.Optional(k): text_sensor.text_sensor_schema() for k in list(UNIT_TEXT) + list(SYSTEM_TEXT)},
        }
    ),
    _validate,
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    for key, idx in UNIT_TEXT.items():
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.set_unit_text_sensor(config[CONF_UNIT], idx, s))
    for key, idx in SYSTEM_TEXT.items():
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(hub.set_system_text_sensor(idx, s))
