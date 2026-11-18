import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import DEVICE_CLASS_RESTART, ENTITY_CATEGORY_CONFIG

from . import CONF_DAIKIN_DIII_ID, DaikinDIII, daikin_diii_ns

DEPENDENCIES = ["daikin_diii"]

ResetAtmegaButton = daikin_diii_ns.class_("ResetAtmegaButton", button.Button, cg.Parented.template(DaikinDIII))

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
        cv.Optional("reset_atmega"): button.button_schema(
            ResetAtmegaButton,
            device_class=DEVICE_CLASS_RESTART,
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    cg.add_global(cg.RawStatement('#include "esphome/components/daikin_diii/controls.h"'))
    if "reset_atmega" in config:
        var = await button.new_button(config["reset_atmega"])
        await cg.register_parented(var, hub)
