"""ESPHome component: passive Jacuzzi J-300 panel-bus decoder (receive only).

The J-300 main board bit-bangs the topside panel bus. The data line is not on
the RJ45, but each bit leaks through the clock (RJ45 pin 4) timing: 15 us
between short clock pulses = 1, 17 us = 0. This component times the clock with
the ESP32 RMT peripheral and decodes the 23 visible slots. See docs/protocol.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]
MULTI_CONF = False

CONF_CLOCK_PIN = "clock_pin"
CONF_WATER_TEMPERATURE = "water_temperature"
CONF_SET_TEMPERATURE = "set_temperature"
CONF_DISPLAY_TEMPERATURE = "display_temperature"
CONF_HEATER = "heater"
CONF_JETS1 = "jets1"
CONF_JETS2 = "jets2"
CONF_SLOT5 = "slot5"
CONF_RAW_FRAME = "raw_frame"

jacuzzi_monitor_ns = cg.esphome_ns.namespace("jacuzzi_monitor")
JacuzziMonitor = jacuzzi_monitor_ns.class_("JacuzziMonitor", cg.Component)

TEMP_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="°F",
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JacuzziMonitor),
        cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_WATER_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_SET_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_DISPLAY_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_HEATER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_HEAT
        ),
        cv.Optional(CONF_JETS1): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING
        ),
        cv.Optional(CONF_JETS2): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING
        ),
        cv.Optional(CONF_SLOT5): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_RAW_FRAME): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_clock_pin(config[CONF_CLOCK_PIN]))

    for key, setter in (
        (CONF_WATER_TEMPERATURE, "set_water_sensor"),
        (CONF_SET_TEMPERATURE, "set_setpoint_sensor"),
        (CONF_DISPLAY_TEMPERATURE, "set_display_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    for key, setter in (
        (CONF_HEATER, "set_heater_sensor"),
        (CONF_JETS1, "set_jets1_sensor"),
        (CONF_JETS2, "set_jets2_sensor"),
        (CONF_SLOT5, "set_slot5_sensor"),
    ):
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    if CONF_RAW_FRAME in config:
        s = await text_sensor.new_text_sensor(config[CONF_RAW_FRAME])
        cg.add(var.set_raw_frame_sensor(s))
