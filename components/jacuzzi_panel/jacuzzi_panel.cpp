#include "jacuzzi_panel.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <Arduino.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/structs/sio.h"
#include "hardware/timer.h"
#include "hardware/structs/sysinfo.h"

#include <initializer_list>

namespace esphome {
namespace jacuzzi_panel {

static const char *const TAG = "jacuzzi_panel";
namespace bus = ::jacuzzi_bus;

static JacuzziPanel *g_panel = nullptr;

static inline bool slot(uint32_t s, int k) { return (s >> (23 - k)) & 1; }

void JacuzziPanel::setup() {
  // Clock and data: plain high-impedance inputs. No pulls, so the bus is not loaded.
  for (uint8_t p : {this->clock_pin_, this->data_pin_}) {
    gpio_init(p);
    gpio_set_dir(p, GPIO_IN);
    gpio_disable_pulls(p);
  }

  // Aux (listen-only) inputs: high-impedance, no pulls.
  for (uint8_t i = 0; i < this->aux_n_; i++) {
    gpio_init(this->aux_[i].pin);
    gpio_set_dir(this->aux_[i].pin, GPIO_IN);
    gpio_disable_pulls(this->aux_[i].pin);
  }

  this->bus_.set_enabled(false);
  if (this->control_ != nullptr) this->control_->publish_state(false);
  if (!this->programs_match_sdk_() || !this->setup_pio_()) {
    this->mark_failed();
    return;
  }
  g_panel = this;
  __sync_synchronize();
  this->core1_go_ = true;  // core 1 starts polling the PIO FIFOs
}

// The PIO programs are hand-encoded (bus_pio.h, shared with the PC simulator). Re-encode
// them with the SDK's encoders and refuse to run if a single word differs.
bool JacuzziPanel::programs_match_sdk_() {
  const uint16_t cap[] = {
      (uint16_t) pio_encode_wait_pin(true, 0),
      (uint16_t) pio_encode_in(pio_pins, 2),
      (uint16_t) pio_encode_mov_not(pio_x, pio_null),
      (uint16_t) pio_encode_jmp_pin(6),
      (uint16_t) pio_encode_in(pio_x, 30),
      (uint16_t) pio_encode_push(false, false),
      (uint16_t) pio_encode_jmp_x_dec(3),
      (uint16_t) pio_encode_jmp(3),
  };
  const uint16_t nop1us = (uint16_t) (pio_encode_mov(pio_y, pio_y) | pio_encode_delay(19));
  const uint16_t prs[] = {
      (uint16_t) pio_encode_pull(false, true),
      (uint16_t) pio_encode_out(pio_x, 32),
      (uint16_t) pio_encode_jmp_not_x(11),
      (uint16_t) pio_encode_wait_pin(true, 0),
      nop1us,
      (uint16_t) pio_encode_jmp_pin(7),
      (uint16_t) pio_encode_jmp(3),
      (uint16_t) pio_encode_wait_pin(false, 0),
      nop1us,
      (uint16_t) pio_encode_jmp_pin(7),
      (uint16_t) pio_encode_jmp_x_dec(2),
      (uint16_t) pio_encode_wait_pin(true, 0),
      (uint16_t) pio_encode_set(pio_pins, 1),
      (uint16_t) pio_encode_set(pio_pindirs, 1),
      nop1us,
      (uint16_t) pio_encode_jmp_pin(19),
      (uint16_t) pio_encode_set(pio_pindirs, 0),
      (uint16_t) pio_encode_set(pio_pins, 0),
      (uint16_t) pio_encode_jmp(11),
      (uint16_t) pio_encode_wait_pin(false, 0),
      (uint16_t) pio_encode_set(pio_pindirs, 0),
      (uint16_t) pio_encode_set(pio_pins, 0),
      (uint16_t) pio_encode_push(false, false),
  };
  static_assert(sizeof(cap) == sizeof(bus::CAPTURE_PROGRAM), "capture program length");
  static_assert(sizeof(prs) == sizeof(bus::PRESS_PROGRAM), "press program length");
  bool ok = true;
  for (size_t i = 0; i < sizeof(cap) / 2; i++)
    if (cap[i] != bus::CAPTURE_PROGRAM[i]) {
      ESP_LOGE(TAG, "capture program word %u: 0x%04X != SDK 0x%04X", (unsigned) i, bus::CAPTURE_PROGRAM[i], cap[i]);
      ok = false;
    }
  for (size_t i = 0; i < sizeof(prs) / 2; i++)
    if (prs[i] != bus::PRESS_PROGRAM[i]) {
      ESP_LOGE(TAG, "press program word %u: 0x%04X != SDK 0x%04X", (unsigned) i, bus::PRESS_PROGRAM[i], prs[i]);
      ok = false;
    }
  return ok;
}

bool JacuzziPanel::setup_pio_() {
  static const pio_program_t cap_prog = {
      .instructions = bus::CAPTURE_PROGRAM,
      .length = sizeof(bus::CAPTURE_PROGRAM) / 2,
      .origin = -1,
      .pio_version = 0,
#if PICO_PIO_VERSION > 0
      .used_gpio_ranges = 0x1,
#endif
  };
  static const pio_program_t press_prog = {
      .instructions = bus::PRESS_PROGRAM,
      .length = sizeof(bus::PRESS_PROGRAM) / 2,
      .origin = -1,
      .pio_version = 0,
#if PICO_PIO_VERSION > 0
      .used_gpio_ranges = 0x1,
#endif
  };
  if (!pio_claim_free_sm_and_add_program(&cap_prog, &this->cap_pio_, &this->cap_sm_, &this->cap_off_)) {
    ESP_LOGE(TAG, "No free PIO state machine / space for the capture program");
    return false;
  }
  if (!pio_claim_free_sm_and_add_program(&press_prog, &this->press_pio_, &this->press_sm_, &this->press_off_)) {
    ESP_LOGE(TAG, "No free PIO state machine / space for the press program");
    return false;
  }
  const float div = (float) clock_get_hz(clk_sys) / (float) bus::SM_CLOCK_HZ;
  this->sm_hz_ = (uint32_t) ((float) clock_get_hz(clk_sys) / div);

  // Capture: in_base = clock (data = clock + 1), jmp_pin = clock, ISR shifts left, RX FIFO joined.
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, this->cap_off_ + bus::CAPTURE_WRAP_TARGET, this->cap_off_ + bus::CAPTURE_WRAP);
  sm_config_set_in_pin_base(&c, this->clock_pin_);
  sm_config_set_jmp_pin(&c, this->clock_pin_);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, div);
  pio_sm_init(this->cap_pio_, this->cap_sm_, this->cap_off_, &c);

  // Press: the reply pin belongs to this SM. Start released (high-Z, latch low).
  pio_gpio_init(this->press_pio_, this->reply_pin_);
  // pio_gpio_init re-enables the input buffer; turn it back off (RP2350 erratum E9 can hold
  // an undriven pad near 2 V, which the main board would read as a held button).
  gpio_set_input_enabled(this->reply_pin_, false);
  gpio_disable_pulls(this->reply_pin_);
  gpio_set_slew_rate(this->reply_pin_, GPIO_SLEW_RATE_SLOW);
  gpio_set_drive_strength(this->reply_pin_, this->reply_drive_ma_ >= 12  ? GPIO_DRIVE_STRENGTH_12MA
                                            : this->reply_drive_ma_ >= 8 ? GPIO_DRIVE_STRENGTH_8MA
                                            : this->reply_drive_ma_ >= 4 ? GPIO_DRIVE_STRENGTH_4MA
                                                                         : GPIO_DRIVE_STRENGTH_2MA);
  pio_sm_config p = pio_get_default_sm_config();
  sm_config_set_wrap(&p, this->press_off_ + bus::PRESS_WRAP_TARGET, this->press_off_ + bus::PRESS_WRAP);
  sm_config_set_in_pin_base(&p, this->clock_pin_);
  sm_config_set_jmp_pin(&p, this->clock_pin_);
  sm_config_set_set_pins(&p, this->reply_pin_, 1);
  sm_config_set_out_shift(&p, true, false, 32);
  sm_config_set_clkdiv(&p, div);
  pio_sm_init(this->press_pio_, this->press_sm_, this->press_off_, &p);
  pio_sm_set_consecutive_pindirs(this->press_pio_, this->press_sm_, this->reply_pin_, 1, false);
  pio_sm_exec(this->press_pio_, this->press_sm_, pio_encode_set(pio_pins, 0));

  pio_sm_set_enabled(this->cap_pio_, this->cap_sm_, true);
  pio_sm_set_enabled(this->press_pio_, this->press_sm_, true);
  return true;
}

void JacuzziPanel::add_aux_input(uint8_t pin, const char *label, binary_sensor::BinarySensor *level,
                                 sensor::Sensor *edges) {
  if (this->aux_n_ >= MAX_AUX) return;
  AuxInput &a = this->aux_[this->aux_n_++];
  a.pin = pin;
  a.label = label;
  a.level = level;
  a.edges = edges;
}

void JacuzziPanel::attach_aux_interrupts_() {
  for (uint8_t i = 0; i < this->aux_n_; i++)
    attachInterruptParam(digitalPinToInterrupt(this->aux_[i].pin), JacuzziPanel::aux_isr_, CHANGE, &this->aux_[i]);
}

// Core 1: count edges on a listen-only pin.
void IRAM_ATTR JacuzziPanel::aux_isr_(void *arg) { static_cast<AuxInput *>(arg)->count++; }

void JacuzziPanel::poll_aux_(uint32_t now) {
  if (now - this->last_aux_ms_ < 1000) return;  // levels at most 1/s; edge counts every 10 s
  const bool report_edges = (now / 10000) != (this->last_aux_ms_ / 10000);
  this->last_aux_ms_ = now;
  const uint32_t in = sio_hw->gpio_in;
  for (uint8_t i = 0; i < this->aux_n_; i++) {
    AuxInput &a = this->aux_[i];
    const int8_t lvl = (in >> a.pin) & 1;
    if (lvl != a.last_level) {
      a.last_level = lvl;
      if (a.level != nullptr) a.level->publish_state(lvl);
    }
    const uint32_t c = a.count;
    if (report_edges && c != a.reported) {
      if (a.reported != 0xFFFFFFFF && c != a.reported)
        ESP_LOGI(TAG, "%s (GP%u): %u edges in last 10 s, level %d", a.label, a.pin, (unsigned) (c - a.reported), lvl);
      a.reported = c;
      if (a.edges != nullptr) a.edges->publish_state(c);
    }
  }
}

// Core 1 only: put the press SM back to "waiting for an arm word" with the pin released.
void IRAM_ATTR JacuzziPanel::reset_press_sm_() {
  PIO p = this->press_pio_;
  const uint sm = this->press_sm_;
  pio_sm_set_enabled(p, sm, false);
  pio_sm_exec(p, sm, pio_encode_set(pio_pindirs, 0));
  pio_sm_exec(p, sm, pio_encode_set(pio_pins, 0));
  pio_sm_clear_fifos(p, sm);
  pio_sm_restart(p, sm);
  pio_sm_exec(p, sm, pio_encode_jmp(this->press_off_));
  pio_sm_set_enabled(p, sm, true);
  this->press_busy_ = false;
}

// Core 1, in a tight loop: drain the PIO FIFOs. No interrupts are involved in bus timing.
void IRAM_ATTR JacuzziPanel::core1_poll_() {
  // "done" reports first: a scan-position-7 press and the last scan pulse end together.
  while (!pio_sm_is_rx_fifo_empty(this->press_pio_, this->press_sm_)) {
    (void) pio_sm_get(this->press_pio_, this->press_sm_);
    this->bus_.on_injected();
    this->press_busy_ = false;
  }
  while (!pio_sm_is_rx_fifo_empty(this->cap_pio_, this->cap_sm_)) {
    const uint32_t w = pio_sm_get(this->cap_pio_, this->cap_sm_);
    const bool fresh = pio_sm_is_rx_fifo_empty(this->cap_pio_, this->cap_sm_);
    const bus::BusAssembler::Action a =
        this->bus_.on_pulse(bus::capture_width_x10(w, this->sm_hz_), bus::capture_data(w), time_us_32(), fresh);
    if (a.abort) {
      this->reset_press_sm_();
      this->aborts_++;
    }
    if (a.arm) {
      pio_sm_put(this->press_pio_, this->press_sm_, a.skip);
      this->press_busy_ = true;
    }
  }
  // Capture FIFO overflowed (a word was dropped)? Count it and clear the flag.
  const uint32_t stall = 1u << (PIO_FDEBUG_RXSTALL_LSB + this->cap_sm_);
  if (this->cap_pio_->fdebug & stall) {
    this->cap_pio_->fdebug = stall;
    this->overflows_++;
  }
  // Control switched off while a press is armed: cancel it.
  if (this->press_busy_ && !this->bus_.enabled()) this->reset_press_sm_();
}

void JacuzziPanel::set_control_enabled(bool en) {
  if (en && !this->bus_ok_state_) {
    ESP_LOGW(TAG, "Not enabling control: no valid frames from the panel bus");
    en = false;
  }
  this->bus_.set_enabled(en);  // core 1 cancels an armed press when this goes false
  if (!en) {
    this->q_len_ = 0;
    this->press_in_flight_ = false;
  }
  ESP_LOGI(TAG, "Control %s", en ? "ENABLED" : "disabled");
  if (this->control_ != nullptr) this->control_->publish_state(en);
}

void JacuzziPanel::request_press(uint8_t scan_pos) {
  if (!this->bus_.enabled()) {
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
  const uint32_t seq = this->bus_.frame_seq();
  if (seq != this->seen_seq_) {
    this->seen_seq_ = seq;
    this->last_frame_ms_ = now;
    this->handle_frame_(this->bus_.frame(), now);
  }

  const bool ok = (now - this->last_frame_ms_) < BUS_TIMEOUT_MS && this->seen_seq_ != 0;
  if (ok != this->bus_ok_state_) {
    this->bus_ok_state_ = ok;
    if (this->bus_ok_ != nullptr) this->bus_ok_->publish_state(ok);
    if (!ok) {
      ESP_LOGW(TAG, "Panel bus lost");
      if (this->bus_.enabled()) this->set_control_enabled(false);
    }
  }

  // Press queue: one press at a time, with a gap between presses.
  if (this->press_in_flight_ && !this->bus_.press_active()) {
    this->press_in_flight_ = false;
    this->press_done_ms_ = now;
  }
  if (!this->press_in_flight_ && this->q_len_ > 0 && now - this->press_done_ms_ >= PRESS_GAP_MS) {
    const uint8_t pos = this->queue_[this->q_head_];
    __sync_synchronize();
    if (this->bus_.arm_press(pos, PRESS_FRAMES)) {
      this->press_in_flight_ = true;
      ESP_LOGI(TAG, "Pressing scan pos %u", pos);
    }
    this->q_head_ = (this->q_head_ + 1) % sizeof(this->queue_);
    this->q_len_--;
  }

  this->poll_aux_(now);

  if (now - this->last_stats_ms_ >= 30000) {
    this->last_stats_ms_ = now;
    ESP_LOGD(TAG, "frames=%u bad=%u refusals=%u drives=%u aborts=%u fifo_overflows=%u", (unsigned) seq,
             (unsigned) this->bus_.bad(), (unsigned) this->bus_.refusals(), (unsigned) this->bus_.drives(),
             (unsigned) this->aborts_, (unsigned) this->overflows_);
  }
}

void JacuzziPanel::handle_frame_(uint32_t frame24, uint32_t now) {
  // Two identical frames in a row before trusting a value.
  const bool confirmed = frame24 == this->last_frame_;
  this->last_frame_ = frame24;
  if (!confirmed) return;

  const uint32_t s = frame24 & 0x7FFFFF;  // slots 1..23; slot k at bit 23-k (as rj45/components/jacuzzi_monitor)
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
  // RP2350 silicon revision (erratum E9 affects early chips; see docs/pico2w-pinout.md).
  ESP_LOGCONFIG(TAG, "  RP2350 chip revision field: %u",
                (unsigned) ((sysinfo_hw->chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >> SYSINFO_CHIP_ID_REVISION_LSB));
  for (uint8_t i = 0; i < this->aux_n_; i++)
    ESP_LOGCONFIG(TAG, "  Listen-only: %s on GP%u", this->aux_[i].label, this->aux_[i].pin);
  ESP_LOGCONFIG(TAG, "  Clock GP%u, Data GP%u, Reply GP%u (driven only while Control is on, %u mA drive)",
                this->clock_pin_, this->data_pin_, this->reply_pin_, this->reply_drive_ma_);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  PIO setup FAILED: not reading the bus, control unavailable");
  } else {
    ESP_LOGCONFIG(TAG, "  PIO: capture pio%u sm%u, press pio%u sm%u, SM clock %u Hz", (unsigned) pio_get_index(this->cap_pio_),
                  this->cap_sm_, (unsigned) pio_get_index(this->press_pio_), this->press_sm_, (unsigned) this->sm_hz_);
  }
}

void ControlSwitch::write_state(bool state) { this->parent_->set_control_enabled(state); }

}  // namespace jacuzzi_panel
}  // namespace esphome

// arduino-pico starts core 1 when setup1()/loop1() exist. Core 1 only drains the PIO FIFOs
// (and counts edges on the listen-only pins), so Wi-Fi activity on core 0 can't delay it.
void setup1() {
  while (esphome::jacuzzi_panel::g_panel == nullptr || !esphome::jacuzzi_panel::g_panel->ready_for_core1()) delay(1);
  esphome::jacuzzi_panel::g_panel->attach_aux_interrupts_();
}
void loop1() {
  if (esphome::jacuzzi_panel::g_panel != nullptr && esphome::jacuzzi_panel::g_panel->ready_for_core1())
    esphome::jacuzzi_panel::g_panel->core1_poll_();
}
