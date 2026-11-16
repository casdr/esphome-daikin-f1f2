import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate

from . import CONF_DAIKIN_DIII_ID, CONF_UNIT, DaikinDIII, daikin_diii_ns

DEPENDENCIES = ["daikin_diii"]

DaikinClimate = daikin_diii_ns.class_("DaikinClimate", climate.Climate, cg.Parented.template(DaikinDIII))

CONFIG_SCHEMA = climate.climate_schema(DaikinClimate).extend(
    {
        cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
        cv.Required(CONF_UNIT): cv.hex_uint8_t,  # group master address
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    cg.add_global(cg.RawStatement('#include "esphome/components/daikin_diii/daikin_climate.h"'))
    var = await climate.new_climate(config)
    await cg.register_parented(var, hub)
    cg.add(var.set_group(config[CONF_UNIT]))
    cg.add(hub.set_unit_climate(config[CONF_UNIT], var))
