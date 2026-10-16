#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/rmt_rx.h>

#include <cstdint>
#include <string>

namespace esphome {
namespace jacuzzi_monitor {

// Passive decoder for the J-300 panel bus clock on RJ45 pin 4. Receive only:
// nothing in this component ever drives a pin. See docs/protocol.md.
class JacuzziMonitor : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_clock_pin(int pin) { clock_pin_ = pin; }
  void set_water_sensor(sensor::Sensor *s) { water_ = s; }
  void set_setpoint_sensor(sensor::Sensor *s) { setpoint_ = s; }
  void set_display_sensor(sensor::Sensor *s) { display_ = s; }
  void set_heater_sensor(binary_sensor::BinarySensor *s) { heater_ = s; }
  void set_jets1_sensor(binary_sensor::BinarySensor *s) { jets1_ = s; }
  void set_jets2_sensor(binary_sensor::BinarySensor *s) { jets2_ = s; }
  void set_slot5_sensor(binary_sensor::BinarySensor *s) { slot5_ = s; }
  void set_raw_frame_sensor(text_sensor::TextSensor *s) { raw_ = s; }

 protected:
  // RMT: 0.1 us ticks; up to 96 symbols covers a 46-pulse header+frame group.
  static constexpr uint32_t RMT_RESOLUTION_HZ = 10'000'000;
  static constexpr size_t RMT_SYMBOLS = 96;
  static constexpr size_t SLOTS = 23;           // visible bits per frame
  static constexpr uint32_t STABLE_MS = 5000;   // display unchanged this long = settled (water temp)
  static constexpr uint32_t REVERT_MIN_MS = 2500;  // set-point display reverts ~3.0 s
  static constexpr uint32_t REVERT_MAX_MS = 4800;  // after the last +/- press (seen 3.0-3.9 s live)

  static bool on_recv_done_(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *arg);
  void arm_();
  bool decode_(const rmt_symbol_word_t *sym, size_t n, uint32_t &bits);
  bool decode_forward_(const uint32_t *start, const uint32_t *width, size_t pulses, uint32_t &bits);
  void handle_frame_(uint32_t bits);
  void track_temperature_(int disp, uint32_t now);
  void dump_capture_(const rmt_symbol_word_t *sym, size_t n);

  int clock_pin_{-1};
  rmt_channel_handle_t chan_{nullptr};
  QueueHandle_t queue_{nullptr};
  rmt_symbol_word_t buf_[RMT_SYMBOLS]{};
  rmt_receive_config_t rx_cfg_{};

  // Frame debounce: publish only after two identical valid frames in a row.
  uint32_t last_bits_{0xFFFFFFFF};
  uint32_t published_bits_{0xFFFFFFFF};

  // Display tracking (water temp vs. set point shown after a +/- press).
  int disp_{-1};
  uint32_t disp_since_{0};
  int prev_val_{-1};
  uint32_t prev_held_{0};
  bool settled_{false};
  int water_val_{-1};
  int set_val_{-1};

  // Stats
  uint32_t ok_{0}, bad_{0}, groups_{0}, last_stats_{0}, last_dump_{0}, fwd_{0};

  sensor::Sensor *water_{nullptr}, *setpoint_{nullptr}, *display_{nullptr};
  binary_sensor::BinarySensor *heater_{nullptr}, *jets1_{nullptr}, *jets2_{nullptr}, *slot5_{nullptr};
  text_sensor::TextSensor *raw_{nullptr};
};

}  // namespace jacuzzi_monitor
}  // namespace esphome
