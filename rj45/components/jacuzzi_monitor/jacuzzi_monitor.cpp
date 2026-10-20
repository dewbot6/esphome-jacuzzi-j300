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
      if (millis() - last_dump_ >= 5000) {
        last_dump_ = millis();
        dump_capture_(buf_, n);
      }
    }
    arm_();
  }

  uint32_t now = millis();
  if (now - last_stats_ >= 30000) {
    last_stats_ = now;
    ESP_LOGD(TAG, "groups=%u frames ok=%u (fwd %u) bad=%u", (unsigned) groups_, (unsigned) ok_, (unsigned) fwd_,
             (unsigned) bad_);
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
  // The frame normally ends with 7 wide (~7 us) pulses; the short (~2 us) pulses
  // before them carry the data. Through the receiver the wide pulses vanish when
  // slot 23 is 1 (every odd units digit), leaving short pulses only.
  bool has_wide = pulses >= 7 + 1;
  for (size_t i = pulses >= 7 ? pulses - 7 : 0; has_wide && i < pulses; i++)
    if (width[i] < 55 || width[i] > 90)
      has_wide = false;
  if (!has_wide)
    return decode_forward_(start, width, pulses, bits);
  const size_t w0 = pulses - 7;
  size_t first_short = w0;
  while (first_short > 0 && width[first_short - 1] >= 12 && width[first_short - 1] <= 35)
    first_short--;
  if (w0 - first_short < 1)
    return false;

  // Through the RS-485 receiver, a clock pulse that follows a 15 us ('1') gap is lost
  // (the line is only weakly driven then), so short pulse k is present only when
  // slot k-1 is 0. Between two observed pulses j < m the gap is 17 + 15*n us, with
  // slots j..m-2 = 1 and slot m-1 = 0. Walk back from the first wide pulse, which
  // follows pulse 24 by 17 us (call it pulse 25). On the Logic 8 (unloaded line) every
  // pulse is present; see rj45/docs/rj45-timing-decode.md.
  bits = 0;
  int m = 25;
  for (size_t i = w0; i > first_short && m > 1; i--) {
    const int32_t g = static_cast<int32_t>(start[i] - start[i - 1]);  // 0.1 us ticks
    const int n = (g - 170 + 75) / 150;                               // round((g - 17 us) / 15 us)
    const int32_t err = g - (170 + 150 * n);
    const int j = m - n - 1;
    if (n < 0 || err < -25 || err > 25 || j < 1)
      break;
    for (int k = j; k <= m - 2; k++)
      bits |= 1u << (23 - k);
    m = j;
  }
  // Pulse 1 or 2 must be the earliest one seen (pulse 1 is missing when the hidden
  // first bit is 1). Anything else means a stretched gap (PIC interrupt) — drop it.
  return m <= 2;
}

// Fallback for captures without the wide pulses (slot 23 = 1). Walk forward from
// the first short pulse, which is pulse 2 (pulse 1 is missing: hidden first bit = 1;
// assuming pulse 1 gives impossible BCD on real captures). Pulses after the last one
// seen are missing, so the trailing slots are 1.
bool JacuzziMonitor::decode_forward_(const uint32_t *start, const uint32_t *width, size_t pulses,
                                     uint32_t &bits) {
  if (pulses < 12)
    return false;
  for (size_t i = 0; i < pulses; i++)
    if (width[i] < 12 || width[i] > 35)
      return false;
  bits = 0;
  int m = 2;
  for (size_t i = 1; i < pulses; i++) {
    const int32_t g = static_cast<int32_t>(start[i] - start[i - 1]);
    const int n = (g - 170 + 75) / 150;
    const int32_t err = g - (170 + 150 * n);
    const int j = m;
    m = j + n + 1;
    if (n < 0 || err < -25 || err > 25 || m > 24)
      return false;
    for (int k = j; k <= m - 2; k++)
      bits |= 1u << (23 - k);
  }
  for (int k = m; k <= 23; k++)
    bits |= 1u << (23 - k);
  // Sanity: the display digits must be valid BCD and slot 23 must be 1 for this path.
  if (((bits >> 8) & 0xF) > 9 || ((bits >> 4) & 0xF) > 9 || (bits & 0xF) > 9 || !(bits & 1))
    return false;
  fwd_++;
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
// (the first press only shows it) and reverts to water temperature ~3.0 s after the
// last press. Decide once the display has settled (unchanged 4 s): the settled value
// is water temperature. If the value before it was held ~3 s, the display reverted and
// that value was the set point; if it was held briefly, a press landed on the settled
// value itself (set point == water temp, so the revert was invisible).
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
  int sp = -1;
  if (prev_val_ >= 0 && prev_val_ != disp) {
    if (prev_held_ >= REVERT_MIN_MS && prev_held_ <= REVERT_MAX_MS)
      sp = prev_val_;
    else if (prev_held_ < REVERT_MIN_MS)
      sp = disp;
  }
  if (sp >= 0 && sp != set_val_) {
    set_val_ = sp;
    ESP_LOGI(TAG, "set point %d", set_val_);
    if (setpoint_ != nullptr)
      setpoint_->publish_state(set_val_);
  }
}

// Debug: log the raw RMT capture (level:duration in 0.1 us ticks) of a failed frame.
void JacuzziMonitor::dump_capture_(const rmt_symbol_word_t *sym, size_t n) {
  std::string out;
  char tmp[16];
  size_t shown = 0;
  for (size_t i = 0; i < n && shown < 64; i++) {
    snprintf(tmp, sizeof(tmp), "%u:%u ", (unsigned) sym[i].level0, (unsigned) sym[i].duration0);
    out += tmp;
    snprintf(tmp, sizeof(tmp), "%u:%u ", (unsigned) sym[i].level1, (unsigned) sym[i].duration1);
    out += tmp;
    shown++;
  }
  ESP_LOGD(TAG, "bad capture: %u symbols: %s", (unsigned) n, out.c_str());
}

void JacuzziMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Jacuzzi J-300 panel-bus decoder (receive only)");
  ESP_LOGCONFIG(TAG, "  Clock pin: GPIO%d (RMT, %u Hz)", clock_pin_, (unsigned) RMT_RESOLUTION_HZ);
  if (is_failed())
    ESP_LOGE(TAG, "  RMT setup failed");
}

}  // namespace jacuzzi_monitor
}  // namespace esphome
