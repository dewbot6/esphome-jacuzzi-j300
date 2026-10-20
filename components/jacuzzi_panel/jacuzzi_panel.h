#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "panel_decoder.h"

#include <cstdint>

namespace esphome {
namespace jacuzzi_panel {

// Reads the J-300 topside panel bus on J1-A directly (clock + data) and, only while
// the Control switch is on, presses panel buttons by driving the reply line during
// that button's scan pulse. See docs/pico2w-pinout.md. Runs on a Raspberry Pi Pico 2 W;
// the bus interrupt is serviced on core 1, away from Wi-Fi.
class JacuzziPanel : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_pins(uint8_t clock, uint8_t data, uint8_t reply) { clock_pin_ = clock; data_pin_ = data; reply_pin_ = reply; }
  void set_water_sensor(sensor::Sensor *s) { water_ = s; }
  void set_setpoint_sensor(sensor::Sensor *s) { setpoint_ = s; }
  void set_display_sensor(sensor::Sensor *s) { display_ = s; }
  void set_heater_sensor(binary_sensor::BinarySensor *s) { heater_ = s; }
  void set_jets1_sensor(binary_sensor::BinarySensor *s) { jets1_ = s; }
  void set_jets2_sensor(binary_sensor::BinarySensor *s) { jets2_ = s; }
  void set_slot5_sensor(binary_sensor::BinarySensor *s) { slot5_ = s; }
  void set_bus_ok_sensor(binary_sensor::BinarySensor *s) { bus_ok_ = s; }
  void set_raw_frame_sensor(text_sensor::TextSensor *s) { raw_ = s; }
  void set_control_switch(switch_::Switch *s) { control_ = s; }
  // Listen-only inputs for unidentified J1-A pins: level + edge count. Never driven.
  void add_aux_input(uint8_t pin, const char *label, binary_sensor::BinarySensor *level, sensor::Sensor *edges);

  // Control. Presses are ignored unless control is enabled.
  void set_control_enabled(bool en);
  void request_press(uint8_t scan_pos);

  // Core-1 side (called from setup1 / the GPIO interrupt).
  void attach_bus_interrupt_();
  static void bus_isr_();
  bool ready_for_core1() const { return core1_go_; }
  void attach_aux_interrupts_();
  static void aux_isr_(void *arg);

 protected:
  static constexpr uint8_t PRESS_FRAMES = 3;        // hold a press for 3 frames (~300 ms)
  static constexpr uint32_t PRESS_GAP_MS = 400;     // idle between queued presses
  static constexpr uint32_t BUS_TIMEOUT_MS = 2000;  // no frames for this long = bus lost
  static constexpr uint32_t STABLE_MS = 5000;       // display unchanged this long = water temp
  static constexpr uint32_t REVERT_MIN_MS = 2500;   // set-point display reverts ~3 s after
  static constexpr uint32_t REVERT_MAX_MS = 4800;   // the last +/- press

  void handle_frame_(uint32_t frame24, uint32_t now);
  void track_temperature_(int disp, uint32_t now);
  void release_reply_();
  void poll_aux_(uint32_t now);

  struct AuxInput {
    uint8_t pin{0};
    const char *label{""};
    binary_sensor::BinarySensor *level{nullptr};
    sensor::Sensor *edges{nullptr};
    volatile uint32_t count{0};
    uint32_t reported{0xFFFFFFFF};
    int8_t last_level{-1};
  };
  static constexpr uint8_t MAX_AUX = 4;
  AuxInput aux_[MAX_AUX];
  uint8_t aux_n_{0};
  uint32_t last_aux_ms_{0};

  uint8_t clock_pin_{2}, data_pin_{3}, reply_pin_{4};
  volatile bool core1_go_{false};

  ::jacuzzi_bus::PanelDecoder decoder_;
  uint32_t seen_seq_{0}, last_frame_ms_{0}, last_stats_ms_{0};
  uint32_t last_frame_{0xFFFFFFFF}, published_{0xFFFFFFFF};
  bool bus_ok_state_{false};

  // Press queue (core 0).
  uint8_t queue_[8]{};
  uint8_t q_head_{0}, q_len_{0};
  uint32_t press_done_ms_{0};
  bool press_in_flight_{false};

  // Display tracking (same rules as components/jacuzzi_monitor).
  int disp_{-1}, prev_val_{-1}, water_val_{-1}, set_val_{-1};
  uint32_t disp_since_{0}, prev_held_{0};
  bool settled_{false};

  sensor::Sensor *water_{nullptr}, *setpoint_{nullptr}, *display_{nullptr};
  binary_sensor::BinarySensor *heater_{nullptr}, *jets1_{nullptr}, *jets2_{nullptr}, *slot5_{nullptr}, *bus_ok_{nullptr};
  text_sensor::TextSensor *raw_{nullptr};
  switch_::Switch *control_{nullptr};
};

class ControlSwitch : public switch_::Switch, public Parented<JacuzziPanel> {
 protected:
  void write_state(bool state) override;
};

class PressButton : public button::Button, public Parented<JacuzziPanel> {
 public:
  void set_scan_pos(uint8_t pos) { pos_ = pos; }

 protected:
  void press_action() override { this->parent_->request_press(this->pos_); }
  uint8_t pos_{0};
};

}  // namespace jacuzzi_panel
}  // namespace esphome
