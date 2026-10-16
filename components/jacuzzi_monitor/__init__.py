"""ESPHome component: Jacuzzi J355 Balboa RS-485 bus participant."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["uart"]
MULTI_CONF = False

jacuzzi_monitor_ns = cg.esphome_ns.namespace("jacuzzi_monitor")
JacuzziMonitor = jacuzzi_monitor_ns.class_(
    "JacuzziMonitor", cg.Component, uart.UARTDevice
)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(JacuzziMonitor)})
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
