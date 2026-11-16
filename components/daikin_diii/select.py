import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import CONF_DAIKIN_DIII_ID, CONF_UNIT, DaikinDIII, daikin_diii_ns

DEPENDENCIES = ["daikin_diii"]

DaikinAirflowSelect = daikin_diii_ns.class_("DaikinAirflowSelect", select.Select, cg.Parented.template(DaikinDIII))
DaikinOutletPairSelect = daikin_diii_ns.class_(
    "DaikinOutletPairSelect", select.Select, cg.Component, cg.Parented.template(DaikinDIII)
)

# Index order matters: airflow_select.h maps index 0 to swing (bus code 7) and
# index 1..5 to positions 0..4 (bus codes 0..4).
AIRFLOW_OPTIONS = [
    "Swing",
    "Position 0 (horizontal)",
    "Position 1",
    "Position 2",
    "Position 3",
    "Position 4 (down)",
]
# Index 0 = normal (b8 F4), 1 = fixed 0 (b8 FF), see DaikinOutletPairSelect.
OUTLET_PAIR_OPTIONS = ["Normal", "Fixed (0)"]
OUTLET_PAIRS = {"1+3": 0, "2+4": 1}

CONF_PAIR = "pair"

CONFIG_SCHEMA = cv.typed_schema(
    {
        # airflow direction of a group (unit = group master)
        "airflow": select.select_schema(DaikinAirflowSelect, icon="mdi:arrow-oscillating").extend(
            {
                cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
                cv.Required(CONF_UNIT): cv.hex_uint8_t,
            }
        ),
        # per-outlet flap setting of one unit (master or member), for a pair of outlets
        "outlet_pair": select.select_schema(DaikinOutletPairSelect, icon="mdi:air-filter")
        .extend(
            {
                cv.GenerateID(CONF_DAIKIN_DIII_ID): cv.use_id(DaikinDIII),
                cv.Required(CONF_UNIT): cv.hex_uint8_t,
                cv.Required(CONF_PAIR): cv.enum(OUTLET_PAIRS),
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
    },
    default_type="airflow",
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_DAIKIN_DIII_ID])
    cg.add_global(cg.RawStatement('#include "esphome/components/daikin_diii/airflow_select.h"'))
    if config["type"] == "outlet_pair":
        var = await select.new_select(config, options=OUTLET_PAIR_OPTIONS)
        await cg.register_component(var, config)
        await cg.register_parented(var, hub)
        cg.add(var.set_unit(config[CONF_UNIT]))
        cg.add(var.set_pair(config[CONF_PAIR]))
        return
    var = await select.new_select(config, options=AIRFLOW_OPTIONS)
    await cg.register_parented(var, hub)
    cg.add(var.set_group(config[CONF_UNIT]))
    cg.add(hub.set_unit_airflow_select(config[CONF_UNIT], var))
