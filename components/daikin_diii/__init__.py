"""Daikin F1F2 (DIII-NET) via a P1P2MQTT bridge's ATmega (P1P2Monitor F1F2 build).

The ATmega does all bus I/O; this component reads its serial lines (250000
baud), decodes frames (see docs/protocol.md) and can write raw frames.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = []
DEPENDENCIES = ["uart", "network"]
MULTI_CONF = False

CONF_DAIKIN_DIII_ID = "daikin_diii_id"
CONF_RESET_PIN = "reset_pin"
CONF_SERIAL_ENABLE_PIN = "serial_enable_pin"
CONF_RAW_MQTT = "raw_mqtt"
CONF_ISP = "isp"
CONF_UNITS = "units"
CONF_ADDRESS = "address"
CONF_B1_INDEX = "b1_index"
CONF_GROUP = "group"
CONF_UNIT = "unit"
CONF_HYDROBOX = "hydrobox"
CONF_ON_UNKNOWN_FRAME = "on_unknown_frame"

daikin_diii_ns = cg.esphome_ns.namespace("daikin_diii")
DaikinDIII = daikin_diii_ns.class_("DaikinDIII", cg.Component, uart.UARTDevice)
UnknownFrameTrigger = daikin_diii_ns.class_(
    "UnknownFrameTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)

UNIT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.hex_uint8_t,
        cv.Optional(CONF_B1_INDEX): cv.hex_uint8_t,
        cv.Optional(CONF_GROUP): cv.hex_uint8_t,
        # a hydrobox takes 35-byte operation commands (power, mode)
        cv.Optional(CONF_HYDROBOX, default=False): cv.boolean,
    }
)

def _not_itm_address(value):
    if value == 0x0A:
        raise cv.Invalid("0x0A is the iTM's address (causes iTM error MC 'address duplication')")
    return value


def _validate_isp(config):
    if CONF_ISP in config and (CONF_RESET_PIN not in config or CONF_SERIAL_ENABLE_PIN not in config):
        raise cv.Invalid("isp needs reset_pin (RESET) and serial_enable_pin (SCK)", path=[CONF_ISP])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DaikinDIII),
            # runs with `frame` (hex) and `frame_type` for an unknown frame type: the first time,
            # then at most once an hour while it keeps coming
            cv.Optional(CONF_ON_UNKNOWN_FRAME): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(UnknownFrameTrigger)}
            ),
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_SERIAL_ENABLE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RAW_MQTT): cv.Schema(
                {
                    cv.Required("broker"): cv.string_strict,
                    cv.Optional("port", default=1883): cv.port,
                    cv.Optional("username", default=""): cv.string,
                    cv.Optional("password", default=""): cv.string,
                    cv.Optional("client_id", default="daikin-f1f2-raw"): cv.string,
                    cv.Optional("topic_prefix", default="P1P2/"): cv.string,
                    cv.Optional("topic_suffix", default="/P1P2MQTT/bridge0"): cv.string,
                }
            ),
            # ATmega programming over ISP (avrdude -c avrisp -P net:<bridge>:<port>).
            # SCK is the serial-enable pin, RESET the reset pin: both required.
            cv.Optional(CONF_ISP): cv.Schema(
                {
                    cv.Required("mosi_pin"): pins.gpio_output_pin_schema,
                    cv.Required("miso_pin"): pins.gpio_input_pin_schema,
                    cv.Optional("port", default=328): cv.port,
                    cv.Optional("window", default="10min"): cv.All(
                        cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(minutes=1))
                    ),
                }
            ),
            cv.Optional("source_address", default=0x30): cv.All(cv.hex_uint8_t, _not_itm_address),
            # Daikin: max. 7000 control commands per indoor unit per year
            # (Modbus Interface DIII design guide). burst: 0 disables the budget.
            cv.Optional("write_budget", default={}): cv.Schema(
                {
                    cv.Optional("burst", default=30): cv.int_range(min=0, max=255),  # saved as one byte
                    cv.Optional("per_year", default=7000): cv.int_range(min=8, max=1000000),  # below 8 the refill period overflows 32 bits
                }
            ),
            cv.Required(CONF_UNITS): cv.ensure_list(UNIT_SCHEMA),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate_isp,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema("daikin_diii", baud_rate=250000, require_rx=True, require_tx=True)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
    if CONF_SERIAL_ENABLE_PIN in config:
        cg.add(var.set_serial_enable_pin(await cg.gpio_pin_expression(config[CONF_SERIAL_ENABLE_PIN])))
    if CONF_RAW_MQTT in config:
        raw_mqtt = config[CONF_RAW_MQTT]
        cg.add_library("heman/AsyncMqttClient-esphome", "2.0.0")
        cg.add_define("DAIKIN_DIII_RAW_MQTT")
        cg.add(var.set_raw_mqtt(raw_mqtt["broker"], raw_mqtt["port"], raw_mqtt["username"], raw_mqtt["password"],
                                raw_mqtt["client_id"], raw_mqtt["topic_prefix"], raw_mqtt["topic_suffix"]))
    if CONF_ISP in config:
        isp = config[CONF_ISP]
        cg.add_define("DAIKIN_DIII_ISP")
        mosi_pin = await cg.gpio_pin_expression(isp["mosi_pin"])
        miso_pin = await cg.gpio_pin_expression(isp["miso_pin"])
        cg.add(var.set_isp(mosi_pin, miso_pin, isp["port"], isp["window"].total_milliseconds))
    cg.add(var.set_source_address(config["source_address"]))
    budget = config["write_budget"]
    cg.add(var.set_write_budget(budget["burst"], (365 * 24 * 3600 * 1000) // budget["per_year"]))
    for unit in config[CONF_UNITS]:
        addr = unit[CONF_ADDRESS]
        cg.add(var.add_unit(addr, unit.get(CONF_B1_INDEX, 0xFF), unit.get(CONF_GROUP, addr), unit[CONF_HYDROBOX]))
    for conf in config.get(CONF_ON_UNKNOWN_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "frame"), (cg.std_string, "frame_type")], conf)
