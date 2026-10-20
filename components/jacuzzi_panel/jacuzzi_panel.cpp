#include "jacuzzi_panel.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <Arduino.h>
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/timer.h"

#include <initializer_list>

namespace esphome {
namespace jacuzzi_panel {

static const char *const TAG = "jacuzzi_panel";
using Drive = ::jacuzzi_bus::PanelDecoder::Drive;

static JacuzziPanel *g_panel = nullptr;
static uint32_t g_clock_mask = 0, g_data_mask = 0, g_reply_mask = 0;

static inline bool slot(uint32_t s, int k) { return (s >> (23 - k)) & 1; }

void JacuzziPanel::setup() {
  // Clock and data: plain high-impedance inputs. No pulls, so the bus is not loaded.
  for (uint8_t p : {this->clock_pin_, this->data_pin_}) {
    gpio_init(p);
    gpio_set_dir(p, GPIO_IN);
    gpio_disable_pulls(p);
  }
  // Reply: output latch low, output driver OFF (high-Z), no pulls, and the input
  // buffer disabled (RP2350 erratum E9 can otherwise hold an undriven pad near 2 V,
  // which the main board would read as a held button).
  gpio_init(this->reply_pin_);
  gpio_disable_pulls(this->reply_pin_);
  gpio_put(this->reply_pin_, false);
  gpio_set_dir(this->reply_pin_, GPIO_IN);
  gpio_set_input_enabled(this->reply_pin_, false);

  g_clock_mask = 1u << this->clock_pin_;
  g_data_mask = 1u << this->data_pin_;
  g_reply_mask = 1u << this->reply_pin_;
  this->decoder_.set_enabled(false);
  g_panel = this;
  __sync_synchronize();
  this->core1_go_ = true;  // setup1() on core 1 attaches the clock interrupt
  if (this->control_ != nullptr) this->control_->publish_state(false);
}

void JacuzziPanel::attach_bus_interrupt_() {
  attachInterrupt(digitalPinToInterrupt(this->clock_pin_), JacuzziPanel::bus_isr_, CHANGE);
}

// Runs on core 1 for every clock edge.
void IRAM_ATTR JacuzziPanel::bus_isr_() {
  JacuzziPanel *self = g_panel;
  if (self == nullptr) return;
  const uint32_t t = time_us_32();
  const uint32_t in = sio_hw->gpio_in;
  const Drive d = (in & g_clock_mask) ? self->decoder_.on_rise(t, (in & g_data_mask) != 0) : self->decoder_.on_fall(t);
  if (d == Drive::START) {
    sio_hw->gpio_set = g_reply_mask;
    sio_hw->gpio_oe_set = g_reply_mask;
  } else if (d == Drive::STOP) {
    sio_hw->gpio_oe_clr = g_reply_mask;
    sio_hw->gpio_clr = g_reply_mask;
  }
}

void JacuzziPanel::release_reply_() {
  sio_hw->gpio_oe_clr = g_reply_mask;
  sio_hw->gpio_clr = g_reply_mask;
}

void JacuzziPanel::set_control_enabled(bool en) {
  if (en && !this->bus_ok_state_) {
    ESP_LOGW(TAG, "Not enabling control: no valid frames from the panel bus");
    en = false;
  }
  this->decoder_.set_enabled(en);
  if (!en) {
    this->release_reply_();
    this->q_len_ = 0;
    this->press_in_flight_ = false;
  }
  ESP_LOGI(TAG, "Control %s", en ? "ENABLED" : "disabled");
  if (this->control_ != nullptr) this->control_->publish_state(en);
}

void JacuzziPanel::request_press(uint8_t scan_pos) {
  if (!this->decoder_.enabled()) {
    ESP_LOGW(TAG, "Press (scan pos %u) ignored: Control is off", scan_pos);
    return;
  }
  if (this->q_len_ >= sizeof(this->queue_)) {
    ESP_LOGW(TAG, "Press queue full");
    return;
  }
  this->queue_[(this->q_head_ + this->q_len_) % sizeof(this->queue_)] = scan_pos;
  this->q_len_++;
}

void JacuzziPanel::loop() {
  const uint32_t now = millis();

  // New frame from core 1?
  const uint32_t seq = this->decoder_.frame_seq();
  if (seq != this->seen_seq_) {
    this->seen_seq_ = seq;
    this->last_frame_ms_ = now;
    this->handle_frame_(this->decoder_.frame(), now);
  }

  const bool ok = (now - this->last_frame_ms_) < BUS_TIMEOUT_MS && this->seen_seq_ != 0;
  if (ok != this->bus_ok_state_) {
    this->bus_ok_state_ = ok;
    if (this->bus_ok_ != nullptr) this->bus_ok_->publish_state(ok);
    if (!ok) {
      ESP_LOGW(TAG, "Panel bus lost");
      if (this->decoder_.enabled()) this->set_control_enabled(false);
    }
  }

  // Press queue: one press at a time, with a gap between presses.
  if (this->press_in_flight_ && !this->decoder_.press_active()) {
    this->press_in_flight_ = false;
    this->press_done_ms_ = now;
  }
  if (!this->press_in_flight_ && this->q_len_ > 0 && now - this->press_done_ms_ >= PRESS_GAP_MS) {
    const uint8_t pos = this->queue_[this->q_head_];
    __sync_synchronize();
    if (this->decoder_.arm_press(pos, PRESS_FRAMES)) {
      this->press_in_flight_ = true;
      ESP_LOGI(TAG, "Pressing scan pos %u", pos);
    }
    this->q_head_ = (this->q_head_ + 1) % sizeof(this->queue_);
    this->q_len_--;
  }

  if (now - this->last_stats_ms_ >= 30000) {
    this->last_stats_ms_ = now;
    ESP_LOGD(TAG, "frames=%u bad_groups=%u timing_rejects=%u", (unsigned) seq, (unsigned) this->decoder_.bad_groups(),
             (unsigned) this->decoder_.timing_rejects());
  }
}

void JacuzziPanel::handle_frame_(uint32_t frame24, uint32_t now) {
  // Two identical frames in a row before trusting a value.
  const bool confirmed = frame24 == this->last_frame_;
  this->last_frame_ = frame24;
  if (!confirmed) return;

  const uint32_t s = frame24 & 0x7FFFFF;  // slots 1..23; slot k at bit 23-k (as jacuzzi_monitor)
  if (frame24 != this->published_) {
    const bool first = this->published_ == 0xFFFFFFFF;
    const uint32_t changed = first ? 0xFFFFFFFF : (frame24 ^ this->published_);
    char raw[25];
    for (int i = 0; i < 24; i++) raw[i] = (frame24 >> (23 - i)) & 1 ? '1' : '0';
    raw[24] = '\0';
    ESP_LOGI(TAG, "frame %s  heater=%d jets1=%d jets2=%d slot5=%d", raw, slot(s, 4), slot(s, 7), slot(s, 11),
             slot(s, 5));
    if (this->raw_ != nullptr) this->raw_->publish_state(raw);
    auto pub = [&](binary_sensor::BinarySensor *b, int k) {
      if (b != nullptr && (changed & (1u << (23 - k)))) b->publish_state(slot(s, k));
    };
    pub(this->heater_, 4);
    pub(this->slot5_, 5);
    pub(this->jets1_, 7);
    pub(this->jets2_, 11);
    this->published_ = frame24;
  }

  const int hund = (s >> 8) & 0xF, tens = (s >> 4) & 0xF, units = s & 0xF;
  if (hund <= 9 && tens <= 9 && units <= 9) this->track_temperature_(hund * 100 + tens * 10 + units, now);
}

// Display shows water temperature; after a +/- press it shows the set point and
// reverts ~3 s after the last press. Decide once the display has settled.
void JacuzziPanel::track_temperature_(int disp, uint32_t now) {
  if (disp != this->disp_) {
    this->prev_val_ = this->disp_;
    this->prev_held_ = now - this->disp_since_;
    this->disp_ = disp;
    this->disp_since_ = now;
    this->settled_ = false;
    if (this->display_ != nullptr) this->display_->publish_state(disp);
    return;
  }
  if (this->settled_ || now - this->disp_since_ < STABLE_MS) return;
  this->settled_ = true;
  if (this->water_val_ != disp) {
    this->water_val_ = disp;
    ESP_LOGI(TAG, "water %d", disp);
    if (this->water_ != nullptr) this->water_->publish_state(disp);
  }
  int sp = -1;
  if (this->prev_val_ >= 0 && this->prev_val_ != disp) {
    if (this->prev_held_ >= REVERT_MIN_MS && this->prev_held_ <= REVERT_MAX_MS)
      sp = this->prev_val_;
    else if (this->prev_held_ < REVERT_MIN_MS)
      sp = disp;
  }
  if (sp >= 0 && sp != this->set_val_) {
    this->set_val_ = sp;
    ESP_LOGI(TAG, "set point %d", sp);
    if (this->setpoint_ != nullptr) this->setpoint_->publish_state(sp);
  }
}

void JacuzziPanel::dump_config() {
  ESP_LOGCONFIG(TAG, "Jacuzzi J-300 panel bus (J1-A)");
  ESP_LOGCONFIG(TAG, "  Clock GP%u, Data GP%u, Reply GP%u (driven only while Control is on)", this->clock_pin_,
                this->data_pin_, this->reply_pin_);
}

void ControlSwitch::write_state(bool state) { this->parent_->set_control_enabled(state); }

}  // namespace jacuzzi_panel
}  // namespace esphome

// arduino-pico starts core 1 when setup1()/loop1() exist. Core 1 only services the
// panel-bus interrupt, so Wi-Fi activity on core 0 can't delay it.
void setup1() {
  while (esphome::jacuzzi_panel::g_panel == nullptr || !esphome::jacuzzi_panel::g_panel->ready_for_core1()) delay(1);
  esphome::jacuzzi_panel::g_panel->attach_bus_interrupt_();
}
void loop1() { delay(100); }
