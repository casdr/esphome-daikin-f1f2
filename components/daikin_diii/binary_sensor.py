import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_CONNECTIVITY, DEVICE_CLASS_HEAT, DEVICE_CLASS_POWER, DEVICE_CLASS_RUNNING

from . import CONF_DAIKIN_DIII_ID, CONF_UNIT, DaikinDIII

DEPENDENCIES = ["daikin_diii"]

UNIT_BINARY = {"power": (0, DEVICE_CLASS_POWER), "heating_active": (1, DEVICE_CLASS_HEAT)}
# defrost: E9 page 6A sub-state 07 on an outdoor module (hyp: defrost or oil return)
SYSTEM_BINARY = {"compressor_running": (0, DEVICE_CLASS_RUNNING), "raw_mqtt_connected": (1, DEVICE_CLASS_CONNECTIVITY),
                 "defrost": (2, None)}


def _validate(config):
    if any(k in config for k in UNIT_BINARY) and CONF_UNIT not in config:
        raise cv.Invalid("unit binary sensors require 'unit'")
    if any(k in config for k in SYSTEM_BINARY) and CONF_UNIT in config:
        raise cv.Invalid("system binary sensors must not have 'unit'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
            cv.Optional(CONF_UNIT): cv.hex_uint8_t,
            **{cv.Optional(k): binary_sensor.binary_sensor_schema(device_class=dc) for k, (_, dc) in {**UNIT_BINARY, **SYSTEM_BINARY}.items() if dc},
            cv.Optional("defrost"): binary_sensor.binary_sensor_schema(icon="mdi:snowflake-melt"),
        }
    ),
    _validate,
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    for key, (idx, _) in UNIT_BINARY.items():
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(hub.set_unit_binary_sensor(config[CONF_UNIT], idx, s))
    for key, (idx, _) in SYSTEM_BINARY.items():
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(hub.set_system_binary_sensor(idx, s))
