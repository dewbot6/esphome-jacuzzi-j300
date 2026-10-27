#pragma once
// Platform-free frame assembler for the J1-A panel bus, fed one pulse at a time by the
// PIO capture program (bus_pio.h), and the rules for arming the PIO press program.
// No ESPHome / Pico SDK dependencies: tools/test_bus_sim.cpp runs it on a PC.
//
// Bus (docs/protocol.md): every ~92 ms, 24 short clock pulses (~2 us, 15-17 us apart)
// carry DATA (bit 1 first); then 7 wide pulses (~7 us, 11 us apart) are the button scan.
// Once a second an 8-short + 7-wide header block precedes the frame.

#include <cstdint>

namespace jacuzzi_bus {

class BusAssembler {
 public:
  static constexpr uint32_t GLITCH_X10 = 10;     // < 1 us: noise, ignore
  static constexpr uint32_t SHORT_MAX_X10 = 40;  // short pulse: 1..4 us
  static constexpr uint32_t WIDE_MIN_X10 = 50;   // wide pulse: 5..11 us
  static constexpr uint32_t WIDE_MAX_X10 = 110;
  static constexpr uint32_t GROUP_GAP_US = 1000;
  // Short pulse 1 -> short pulse 24 takes 383 us (23 gaps of 15/17 us). A frame whose
  // count is 24 but whose span is off means the count is wrong: never arm on it.
  static constexpr uint32_t SPAN_MIN_US = 370, SPAN_MAX_US = 396;

  struct Action {
    bool arm{false};    // write `skip` to the press program now
    uint8_t skip{0};    //   = scan position - 1
    bool abort{false};  // reset the press program now (armed, but the frame went wrong)
  };

  // One captured pulse. `arrival_us` = when it was read (just after its falling edge);
  // `fresh` = nothing else was waiting in the capture FIFO (we are not running late).
  Action on_pulse(uint32_t width_x10, bool data, uint32_t arrival_us, bool fresh) {
    Action a;
    if (width_x10 < GLITCH_X10) return a;
    // A press armed for the frame that just ended and still not reported: cancel it.
    if (frame_done_armed_) { frame_done_armed_ = false; if (armed_) { armed_ = false; a.abort = true; } }
    if (have_last_ && arrival_us - last_arrival_ > GROUP_GAP_US) a.abort |= reset_block_();
    have_last_ = true;
    last_arrival_ = arrival_us;

    if (width_x10 <= SHORT_MAX_X10) {
      if (wides_ > 0) a.abort |= reset_block_();  // previous block (header/frame) is over
      if (shorts_ >= 24) { bad_++; a.abort |= reset_block_(); return a; }
      if (shorts_ == 0) block_start_ = arrival_us;
      bits_ = (bits_ << 1) | (data ? 1u : 0u);
      shorts_++;
      if (shorts_ == 24 && enabled_ && press_pos_ != 0) {
        const uint32_t span = arrival_us - block_start_;
        if (fresh && span >= SPAN_MIN_US && span <= SPAN_MAX_US && press_windows_ > 0) {
          a.arm = true;
          a.skip = static_cast<uint8_t>(press_pos_ - 1);
          armed_ = true;
          press_windows_--;
        } else {
          refusals_++;
        }
      }
    } else if (width_x10 >= WIDE_MIN_X10 && width_x10 <= WIDE_MAX_X10) {
      wides_++;
      if (wides_ == 7) {
        if (shorts_ == 24) {
          frame_ = bits_;
          frame_seq_++;
          if (injected_) count_injection_();
          // Position 7's "done" comes ~4.6 us after this pulse falls (the press holds through
          // the gap, bus_pio.h): give a late "done" until the next pulse before cancelling.
          if (armed_) frame_done_armed_ = true;
          else if (press_windows_ == 0) press_pos_ = 0;
        }
        injected_ = false;
      } else if (wides_ > 7) {
        bad_++;
        a.abort |= reset_block_();
      }
    } else {
      bad_++;
      a.abort |= reset_block_();
    }
    return a;
  }

  // The press program reported a drive ("done") for this frame.
  void on_injected() {
    if (!armed_) return;
    armed_ = false;
    drives_++;
    if (frame_done_armed_) { frame_done_armed_ = false; count_injection_(); }  // late "done"
    else injected_ = true;
  }

  // --- control (written by the other core; single-word fields) ---
  void set_enabled(bool en) {
    enabled_ = en;
    if (!en) press_pos_ = 0;
  }
  bool enabled() const { return enabled_; }
  // Hold scan position `pos` (1..7) for `frames` frames (at most frames+2 drive windows).
  bool arm_press(uint8_t pos, uint8_t frames) {
    if (!enabled_ || pos < 1 || pos > 7 || press_pos_ != 0) return false;
    press_frames_ = frames;
    press_windows_ = frames + 2;
    press_pos_ = pos;
    return true;
  }
  bool press_active() const { return press_pos_ != 0; }
  bool armed() const { return armed_; }

  // --- results ---
  uint32_t frame() const { return frame_; }  // 24 bits, bit 1 (first sent) = MSB
  uint32_t frame_seq() const { return frame_seq_; }
  uint32_t bad() const { return bad_; }
  uint32_t refusals() const { return refusals_; }
  uint32_t drives() const { return drives_; }

 private:
  void count_injection_() {
    if (press_frames_ > 0 && --press_frames_ == 0) press_pos_ = 0;
    if (press_windows_ == 0) press_pos_ = 0;
  }
  // Returns true if an armed press must be cancelled.
  bool reset_block_() {
    const bool was_armed = armed_;
    shorts_ = 0;
    wides_ = 0;
    bits_ = 0;
    armed_ = false;
    injected_ = false;
    frame_done_armed_ = false;
    return was_armed;
  }

  uint32_t last_arrival_{0}, block_start_{0}, bits_{0};
  bool have_last_{false}, armed_{false}, injected_{false}, frame_done_armed_{false};
  uint8_t shorts_{0}, wides_{0};

  volatile uint32_t frame_{0}, frame_seq_{0};
  uint32_t bad_{0}, refusals_{0}, drives_{0};

  volatile bool enabled_{false};
  volatile uint8_t press_pos_{0}, press_frames_{0}, press_windows_{0};
};

}  // namespace jacuzzi_bus
