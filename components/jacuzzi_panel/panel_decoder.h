#pragma once
// Platform-free decoder for the Jacuzzi J-300 topside panel bus (J1-A), plus the
// timing rules for injecting a button press. No ESPHome or Pico SDK dependencies,
// so tools/test_panel_decoder.cpp can replay Logic 8 captures through it on a PC.
//
// Bus (see docs/protocol.md, docs/j1a-pinout.md): every 100 ms the main board sends
//   24 short clock pulses (~2 us high, 15-17 us apart) - the panel latches DATA on
//      each rising edge (bit 1 first),
//   then 7 wide pulses (~7 us high, 11 us apart) - the button scan: the panel drives
//      the reply line high during the pulse of the pressed button.
// Once a second an extra header block (8 short + 7 wide) precedes the frame.
//
// Times are in microseconds (wrapping uint32_t, e.g. time_us_32()).

#include <cstdint>

namespace jacuzzi_bus {

class PanelDecoder {
 public:
  static constexpr uint32_t GROUP_GAP_US = 1000;  // idle longer than this starts a new group
  static constexpr uint32_t GLITCH_US = 1;        // pulses shorter than this are noise
  static constexpr uint32_t SHORT_MAX_US = 4;     // short <= 4 us < wide
  static constexpr uint32_t WIDE_MAX_US = 11;
  static constexpr uint32_t FIRST_WIDE_US = 17;   // rise of short pulse 24 -> rise of wide 1
  static constexpr uint32_t WIDE_PERIOD_US = 11;  // wide pulse spacing
  static constexpr uint32_t PRESS_TOL_US = 3;     // allowed timing error before we refuse to drive

  // Result of an edge: whether the reply line should be driven high from now on.
  enum class Drive : uint8_t { NO_CHANGE, START, STOP };

  // Rising clock edge. `data` is the DATA line level at this moment.
  Drive on_rise(uint32_t t, bool data) {
    Drive d = NO_CHANGE_STOP_();
    if (in_pulse_) { bad_groups_++; reset_group_(); }  // missed a falling edge
    if (t - last_fall_ > GROUP_GAP_US) reset_group_();
    rise_t_ = t;
    rise_data_ = data;
    in_pulse_ = true;
    // Inject only on the k-th wide pulse of a normal frame (exactly 24 short pulses
    // before it), and only if this edge arrives exactly when that pulse should.
    if (enabled_ && press_pos_ != 0 && shorts_ == 24 && wides_ == press_pos_ - 1) {
      const uint32_t expect = short24_rise_ + FIRST_WIDE_US + WIDE_PERIOD_US * (press_pos_ - 1);
      const int32_t err = static_cast<int32_t>(t - expect);
      if (err >= -static_cast<int32_t>(PRESS_TOL_US) && err <= static_cast<int32_t>(PRESS_TOL_US)) {
        driving_ = true;
        injected_this_frame_ = true;
        // Hard cap on drive windows per press, even if frames keep failing to complete.
        if (press_windows_ == 0 || --press_windows_ == 0) press_pos_ = 0;
        d = Drive::START;
      } else {
        timing_rejects_++;
      }
    }
    return d;
  }

  // Falling clock edge.
  Drive on_fall(uint32_t t) {
    Drive d = NO_CHANGE_STOP_();
    if (!in_pulse_) { bad_groups_++; reset_group_(); return d; }  // missed a rising edge
    in_pulse_ = false;
    const uint32_t w = t - rise_t_;
    if (w < GLITCH_US) return d;  // glitch: ignore entirely (last_fall_ unchanged)
    last_fall_ = t;
    if (w <= SHORT_MAX_US) {
      if (wides_ > 0) { shorts_ = 0; wides_ = 0; bits_ = 0; }  // header done, frame begins
      if (shorts_ >= 24) { bad_groups_++; reset_group_(); return d; }
      bits_ = (bits_ << 1) | (rise_data_ ? 1u : 0u);
      shorts_++;
      if (shorts_ == 24) short24_rise_ = rise_t_;
    } else if (w <= WIDE_MAX_US) {
      wides_++;
      if (wides_ == 7) {
        if (shorts_ == 24) {
          frame_ = bits_;
          frame_seq_++;
          if (injected_this_frame_ && press_frames_ > 0 && --press_frames_ == 0) press_pos_ = 0;
        }
        // shorts_ == 8 is the 1 s header block: not a frame.
        injected_this_frame_ = false;
      } else if (wides_ > 7) {
        bad_groups_++;
        reset_group_();
      }
    } else {
      bad_groups_++;
      reset_group_();
    }
    return d;
  }

  // --- control (call with the decoder's interrupt masked / from the same core) ---
  void set_enabled(bool en) { enabled_ = en; if (!en) { press_pos_ = 0; press_frames_ = 0; } }
  bool enabled() const { return enabled_; }
  // Hold scan position `pos` (1..7) for `frames` consecutive normal frames.
  bool arm_press(uint8_t pos, uint8_t frames) {
    if (!enabled_ || pos < 1 || pos > 7 || press_pos_ != 0) return false;
    press_frames_ = frames;
    press_windows_ = frames + 2;
    press_pos_ = pos;
    return true;
  }
  bool press_active() const { return press_pos_ != 0; }

  // --- results ---
  // Last complete frame: 24 bits, bit 1 (first sent) is the MSB.
  uint32_t frame() const { return frame_; }
  uint32_t frame_seq() const { return frame_seq_; }
  uint32_t bad_groups() const { return bad_groups_; }
  uint32_t timing_rejects() const { return timing_rejects_; }

 private:
  // Any edge ends an injection: the drive window is exactly one wide pulse.
  Drive NO_CHANGE_STOP_() {
    if (driving_) { driving_ = false; return Drive::STOP; }
    return Drive::NO_CHANGE;
  }
  void reset_group_() { shorts_ = 0; wides_ = 0; bits_ = 0; injected_this_frame_ = false; }

  uint32_t rise_t_{0}, last_fall_{0}, short24_rise_{0};
  bool rise_data_{false}, in_pulse_{false}, driving_{false}, injected_this_frame_{false};
  uint8_t shorts_{0}, wides_{0};
  uint32_t bits_{0};

  volatile uint32_t frame_{0}, frame_seq_{0};
  uint32_t bad_groups_{0}, timing_rejects_{0};

  volatile bool enabled_{false};
  volatile uint8_t press_pos_{0}, press_frames_{0}, press_windows_{0};

  static constexpr Drive NO_CHANGE = Drive::NO_CHANGE;
};

}  // namespace jacuzzi_bus
