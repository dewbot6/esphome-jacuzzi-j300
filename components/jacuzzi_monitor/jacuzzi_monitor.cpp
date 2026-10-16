#include "jacuzzi_monitor.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace jacuzzi_monitor {

static const char *const TAG = "jacuzzi_monitor";

// Slot k (1..23) is stored at bit (23 - k), so the frame prints MSB-first as slot 1..23.
static inline bool slot(uint32_t bits, int k) { return (bits >> (23 - k)) & 1; }

void JacuzziMonitor::setup() {
  queue_ = xQueueCreate(1, sizeof(size_t));

  rmt_rx_channel_config_t cfg{};
  cfg.gpio_num = static_cast<gpio_num_t>(clock_pin_);
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = RMT_RESOLUTION_HZ;
  cfg.mem_block_symbols = RMT_SYMBOLS;
  esp_err_t err = rmt_new_rx_channel(&cfg, &chan_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
    mark_failed();
    return;
  }

  rmt_rx_event_callbacks_t cbs{};
  cbs.on_recv_done = on_recv_done_;
  rmt_rx_register_event_callbacks(chan_, &cbs, this);
  rmt_enable(chan_);

  // Pulses < 1 us are relay/light switching glitches; > 1 ms of idle ends a frame
  // (frames are 100 ms apart, the longest gap inside a frame group is ~80 us).
  rx_cfg_.signal_range_min_ns = 1000;
  rx_cfg_.signal_range_max_ns = 1000 * 1000;
  arm_();
}

void JacuzziMonitor::arm_() {
  esp_err_t err = rmt_receive(chan_, buf_, sizeof(buf_), &rx_cfg_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
}

bool IRAM_ATTR JacuzziMonitor::on_recv_done_(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata,
                                             void *arg) {
  auto *self = static_cast<JacuzziMonitor *>(arg);
  BaseType_t woken = pdFALSE;
  size_t n = edata->num_symbols;
  xQueueOverwriteFromISR(self->queue_, &n, &woken);
  return woken == pdTRUE;
}

void JacuzziMonitor::loop() {
  size_t n;
  if (queue_ != nullptr && xQueueReceive(queue_, &n, 0) == pdTRUE) {
    groups_++;
    uint32_t bits;
    if (decode_(buf_, n, bits)) {
      ok_++;
      handle_frame_(bits);
    } else {
      bad_++;
    }
    arm_();
  }

  uint32_t now = millis();
  if (now - last_stats_ >= 30000) {
    last_stats_ = now;
    ESP_LOGD(TAG, "groups=%u frames ok=%u bad=%u", (unsigned) groups_, (unsigned) ok_, (unsigned) bad_);
    if (groups_ == 0)
      ESP_LOGW(TAG, "No clock activity on GPIO%d — check A->pin 4, B->pin 3", clock_pin_);
  }
}

// Turn one RMT capture (a frame, or the 1 s header + frame) into 23 slot bits.
bool JacuzziMonitor::decode_(const rmt_symbol_word_t *sym, size_t n, uint32_t &bits) {
  if (n == 0)
    return false;
  // Capture starts on the first edge, so the first recorded level is the pulse level.
  // This makes the decoder independent of the transceiver's output polarity.
  const uint32_t pulse_level = sym[0].level0;

  uint32_t start[RMT_SYMBOLS], width[RMT_SYMBOLS];
  size_t pulses = 0;
  uint32_t t = 0;
  for (size_t i = 0; i < n && pulses < RMT_SYMBOLS; i++) {
    const uint32_t dur[2] = {sym[i].duration0, sym[i].duration1};
    const uint32_t lvl[2] = {sym[i].level0, sym[i].level1};
    for (int h = 0; h < 2; h++) {
      if (dur[h] == 0)
        goto done;  // end-of-capture marker
      if (lvl[h] == pulse_level && pulses < RMT_SYMBOLS) {
        start[pulses] = t;
        width[pulses] = dur[h];
        pulses++;
      }
      t += dur[h];
    }
  }
done:
  if (pulses < FRAME_PULSES)
    return false;

  // The frame is always the last 31 pulses: 24 short (~2 us) then 7 wide (~7 us).
  const size_t f = pulses - FRAME_PULSES;
  for (size_t i = 0; i < FRAME_PULSES; i++) {
    const uint32_t w = width[f + i];  // 0.1 us ticks
    const bool ok = (i < 24) ? (w >= 12 && w <= 35) : (w >= 55 && w <= 90);
    if (!ok)
      return false;
  }

  // Gap between short-pulse rising edges: 15 us = 1, 17 us = 0. Anything else
  // (an interrupt stretched the PIC's loop) invalidates the frame; the next one
  // arrives in 100 ms.
  bits = 0;
  for (size_t k = 1; k <= SLOTS; k++) {
    const uint32_t d = start[f + k] - start[f + k - 1];
    if (d >= 135 && d < 160)
      bits |= 1u << (23 - k);
    else if (d < 135 || d > 185)
      return false;
  }
  return true;
}

void JacuzziMonitor::handle_frame_(uint32_t bits) {
  // Require two identical frames in a row before trusting a value.
  const bool confirmed = (bits == last_bits_);
  last_bits_ = bits;
  if (!confirmed)
    return;

  if (bits != published_bits_) {
    const bool first = (published_bits_ == 0xFFFFFFFF);
    char raw[SLOTS + 1];
    for (size_t k = 1; k <= SLOTS; k++)
      raw[k - 1] = slot(bits, k) ? '1' : '0';
    raw[SLOTS] = '\0';
    ESP_LOGI(TAG, "frame %s  heater=%d jets1=%d jets2=%d slot5=%d", raw, slot(bits, 4), slot(bits, 7),
             slot(bits, 11), slot(bits, 5));
    if (raw_ != nullptr)
      raw_->publish_state(raw);
    const uint32_t changed = first ? 0xFFFFFFFF : (bits ^ published_bits_);
    auto pub = [&](binary_sensor::BinarySensor *s, int k) {
      if (s != nullptr && (changed & (1u << (23 - k))))
        s->publish_state(slot(bits, k));
    };
    pub(heater_, 4);
    pub(slot5_, 5);
    pub(jets1_, 7);
    pub(jets2_, 11);
    published_bits_ = bits;
  }

  // Display: slots 12-15 hundreds, 16-19 tens, 20-23 units, all BCD.
  const int hund = (bits >> 8) & 0xF, tens = (bits >> 4) & 0xF, units = bits & 0xF;
  if (hund <= 9 && tens <= 9 && units <= 9)
    track_temperature_(hund * 100 + tens * 10 + units, millis());
}

// The display shows water temperature. After a +/- press it shows the set point
// and reverts to water temperature ~3.0 s after the last press. Decide once the
// display has settled: the settled value is water temperature, and if the value
// before it was held for ~3 s, that value was the set point.
void JacuzziMonitor::track_temperature_(int disp, uint32_t now) {
  if (disp != disp_) {
    prev_val_ = disp_;
    prev_held_ = now - disp_since_;
    disp_ = disp;
    disp_since_ = now;
    settled_ = false;
    if (display_ != nullptr)
      display_->publish_state(disp);
    return;
  }
  if (settled_ || now - disp_since_ < STABLE_MS)
    return;
  settled_ = true;
  if (water_val_ != disp) {
    water_val_ = disp;
    ESP_LOGI(TAG, "water %d", water_val_);
    if (water_ != nullptr)
      water_->publish_state(water_val_);
  }
  if (prev_val_ >= 0 && prev_val_ != disp && prev_held_ >= REVERT_MIN_MS && prev_held_ <= REVERT_MAX_MS &&
      set_val_ != prev_val_) {
    set_val_ = prev_val_;
    ESP_LOGI(TAG, "set point %d", set_val_);
    if (setpoint_ != nullptr)
      setpoint_->publish_state(set_val_);
  }
}

void JacuzziMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Jacuzzi J-300 panel-bus decoder (receive only)");
  ESP_LOGCONFIG(TAG, "  Clock pin: GPIO%d (RMT, %u Hz)", clock_pin_, (unsigned) RMT_RESOLUTION_HZ);
  if (is_failed())
    ESP_LOGE(TAG, "  RMT setup failed");
}

}  // namespace jacuzzi_monitor
}  // namespace esphome
