"""ESPHome component: Jacuzzi J-300 topside panel bus on J1-A (Raspberry Pi Pico 2 W).

Reads the panel bus directly (clock + data) and, only while the Control switch is on,
presses panel buttons by driving the reply line during that button's scan pulse.
See docs/pico2w-pinout.md and docs/j1a-pinout.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, button, sensor, switch, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_RESTORE_MODE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

DEPENDENCIES = ["rp2"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "switch", "button"]
MULTI_CONF = False

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_REPLY_PIN = "reply_pin"
CONF_WATER_TEMPERATURE = "water_temperature"
CONF_SET_TEMPERATURE = "set_temperature"
CONF_DISPLAY_TEMPERATURE = "display_temperature"
CONF_HEATER = "heater"
CONF_JETS1 = "jets1"
CONF_JETS2 = "jets2"
CONF_SLOT5 = "slot5"
CONF_BUS_OK = "bus_ok"
CONF_RAW_FRAME = "raw_frame"
CONF_CONTROL = "control"
CONF_TEMP_UP = "temp_up"
CONF_TEMP_DOWN = "temp_down"
CONF_JETS1_BUTTON = "jets1_button"
CONF_JETS2_BUTTON = "jets2_button"

# Button scan positions, from captures/31-j1a-buttons.sal and 32-j1a-jets1.sal.
SCAN_POS = {
    CONF_TEMP_UP: 4,
    CONF_TEMP_DOWN: 3,
    CONF_JETS1_BUTTON: 7,
    CONF_JETS2_BUTTON: 2,
}

ns = cg.esphome_ns.namespace("jacuzzi_panel")
JacuzziPanel = ns.class_("JacuzziPanel", cg.Component)
ControlSwitch = ns.class_("ControlSwitch", switch.Switch, cg.Parented.template(JacuzziPanel))
PressButton = ns.class_("PressButton", button.Button, cg.Parented.template(JacuzziPanel))

TEMP_SCHEMA = sensor.sensor_schema(
    unit_of_measurement="°F",
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)


def _control_always_off(config):
    # Control must come up OFF after every boot, whatever the YAML says.
    if config.get(CONF_RESTORE_MODE, "ALWAYS_OFF") != "ALWAYS_OFF":
        raise cv.Invalid("control must use restore_mode: ALWAYS_OFF")
    return config


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JacuzziPanel),
        cv.Optional(CONF_CLOCK_PIN, default=2): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_DATA_PIN, default=3): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_REPLY_PIN, default=4): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_WATER_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_SET_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_DISPLAY_TEMPERATURE): TEMP_SCHEMA,
        cv.Optional(CONF_HEATER): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_HEAT),
        cv.Optional(CONF_JETS1): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_RUNNING),
        cv.Optional(CONF_JETS2): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_RUNNING),
        cv.Optional(CONF_SLOT5): binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional(CONF_BUS_OK): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_RAW_FRAME): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional(CONF_CONTROL): cv.All(
            switch.switch_schema(
                ControlSwitch,
                icon="mdi:gesture-tap-button",
                entity_category=ENTITY_CATEGORY_CONFIG,
                default_restore_mode="ALWAYS_OFF",
            ),
            _control_always_off,
        ),
        **{
            cv.Optional(key): button.button_schema(PressButton, icon="mdi:gesture-tap")
            for key in SCAN_POS
        },
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_CLOCK_PIN], config[CONF_DATA_PIN], config[CONF_REPLY_PIN]))

    for key, setter in (
        (CONF_WATER_TEMPERATURE, "set_water_sensor"),
        (CONF_SET_TEMPERATURE, "set_setpoint_sensor"),
        (CONF_DISPLAY_TEMPERATURE, "set_display_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await sensor.new_sensor(config[key])))

    for key, setter in (
        (CONF_HEATER, "set_heater_sensor"),
        (CONF_JETS1, "set_jets1_sensor"),
        (CONF_JETS2, "set_jets2_sensor"),
        (CONF_SLOT5, "set_slot5_sensor"),
        (CONF_BUS_OK, "set_bus_ok_sensor"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await binary_sensor.new_binary_sensor(config[key])))

    if CONF_RAW_FRAME in config:
        cg.add(var.set_raw_frame_sensor(await text_sensor.new_text_sensor(config[CONF_RAW_FRAME])))

    if CONF_CONTROL in config:
        sw = await switch.new_switch(config[CONF_CONTROL])
        await cg.register_parented(sw, var)
        cg.add(var.set_control_switch(sw))

    for key, pos in SCAN_POS.items():
        if key in config:
            b = await button.new_button(config[key])
            await cg.register_parented(b, var)
            cg.add(b.set_scan_pos(pos))
