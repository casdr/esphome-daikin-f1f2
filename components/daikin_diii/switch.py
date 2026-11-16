import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_DAIKIN_DIII_ID, DaikinDIII, daikin_diii_ns

DEPENDENCIES = ["daikin_diii"]

WriteEnableSwitch = daikin_diii_ns.class_("WriteEnableSwitch", switch.Switch, cg.Component, cg.Parented.template(DaikinDIII))
RawLoggingSwitch = daikin_diii_ns.class_("RawLoggingSwitch", switch.Switch, cg.Component, cg.Parented.template(DaikinDIII))
AtmegaProgrammingSwitch = daikin_diii_ns.class_(
    "AtmegaProgrammingSwitch", switch.Switch, cg.Component, cg.Parented.template(DaikinDIII)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
        cv.Optional("write_enable"): switch.switch_schema(
            WriteEnableSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:pencil-lock",
            default_restore_mode="RESTORE_DEFAULT_ON",
        ).extend(cv.COMPONENT_SCHEMA),
        cv.Optional("raw_logging"): switch.switch_schema(
            RawLoggingSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:database-arrow-down",
            default_restore_mode="RESTORE_DEFAULT_ON",
        ).extend(cv.COMPONENT_SCHEMA),
        # opens the ATmega programming port (hub `isp:` block) for its window
        cv.Optional("atmega_programming"): switch.switch_schema(
            AtmegaProgrammingSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:chip",
            default_restore_mode="ALWAYS_OFF",
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


def _final_validate(config):
    if "atmega_programming" not in config:
        return config
    hubs = fv.full_config.get().get("daikin_diii", [])
    hubs = hubs if isinstance(hubs, list) else [hubs]
    if not any("isp" in h for h in hubs):
        raise cv.Invalid("atmega_programming needs an `isp:` block on daikin_diii", path=["atmega_programming"])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    cg.add_global(cg.RawStatement('#include "esphome/components/daikin_diii/controls.h"'))
    if "write_enable" in config:
        s = await switch.new_switch(config["write_enable"])
        await cg.register_component(s, config["write_enable"])
        await cg.register_parented(s, hub)
    if "raw_logging" in config:
        r = await switch.new_switch(config["raw_logging"])
        await cg.register_component(r, config["raw_logging"])
        await cg.register_parented(r, hub)
    if "atmega_programming" in config:
        p = await switch.new_switch(config["atmega_programming"])
        await cg.register_component(p, config["atmega_programming"])
        await cg.register_parented(p, hub)
