#pragma once
// PIO programs for the J1-A panel bus, as raw instruction words. Shared by the firmware
// (jacuzzi_panel.cpp loads them with pio_add_program, which relocates JMP targets) and by
// tools/test_bus_sim.cpp, which runs these exact words in a PIO simulator against Logic 8
// recordings. Jump targets below are program-relative.
//
// Both state machines use IN_BASE = clock pin (GP2), so `wait pin 0` / `in pins` see the clock
// as bit 0 and DATA (GP3) as bit 1. Run them at SM_CLOCK_HZ.

#include <cstdint>

namespace jacuzzi_bus {

inline constexpr uint32_t SM_CLOCK_HZ = 20'000'000;  // 50 ns per PIO cycle

// --- Capture: one RX word per clock pulse -----------------------------------------------
// word bit 31 = DATA sampled just after the rising edge, bit 30 = clock (always 1),
// bits 29..0 = ~x: number of 2-cycle loop iterations the clock stayed high.
// Width in PIO cycles ~= 2*count + 3.
//
//   0: wait 1 pin 0        ; clock rises
//   1: in   pins, 2        ; ISR = [data, clk]
//   2: mov  x, ~null       ; x = 0xFFFFFFFF
//   3: jmp  pin, 6         ; clock still high -> count     (loop: 3 -> 6 -> 3, 2 cycles)
//   4: in   x, 30          ; ISR = [data, clk, x(30)]
//   5: push noblock        ; .wrap  (-> 0)
//   6: jmp  x--, 3
//   7: jmp  3              ; only if x ever reaches 0 (never in practice)
inline constexpr uint16_t CAPTURE_PROGRAM[] = {
    0x20A0,  // wait 1 pin 0
    0x4002,  // in pins, 2
    0xA02B,  // mov x, ~null
    0x00C6,  // jmp pin, 6
    0x403E,  // in x, 30
    0x8000,  // push noblock
    0x0043,  // jmp x--, 3
    0x0003,  // jmp 3
};
inline constexpr uint8_t CAPTURE_WRAP_TARGET = 0;
inline constexpr uint8_t CAPTURE_WRAP = 5;
// ISR shifts LEFT, no autopush (in_shiftdir = false).

// --- Press: drive the reply pin high for exactly one wide clock pulse ------------------
// TX word = number of clock pulses to let pass before the one to drive (k-1 for scan
// position k, written right after short pulse 24 ends). Every pulse it counts must stay
// high for ~1 us (and every low must stay low ~1 us), so sub-microsecond glitches on the
// clock line are never counted. It drives from the target pulse's rising edge (~0.1 us),
// re-checks after ~1 us that it really is a pulse (else releases and waits for the real
// one), holds until the falling edge, releases (high-Z) and pushes one RX word ("done").
// SET_BASE = reply pin, SET_COUNT = 1.
//
//   0: pull block              ; wait for an arm word
//   1: out  x, 32
//   2: jmp  !x, 11             ; nothing to skip -> fire
//   3: wait 1 pin 0            ; SKIP: a pulse rises...
//   4: mov  y, y [19]          ;   ~1 us
//   5: jmp  pin, 7             ;   still high: real pulse
//   6: jmp  3                  ;   glitch: ignore
//   7: wait 0 pin 0            ; ...and falls
//   8: mov  y, y [19]          ;   ~1 us
//   9: jmp  pin, 7             ;   high again: it was a dip, keep waiting for the fall
//  10: jmp  x--, 2             ;   one real pulse passed (x > 0 here, so always jumps)
//  11: wait 1 pin 0            ; FIRE: target pulse rises
//  12: set  pins, 1
//  13: set  pindirs, 1         ;   drive high (~0.1 us after the edge)
//  14: mov  y, y [19]          ;   ~1 us
//  15: jmp  pin, 19            ;   still high: it's the real pulse
//  16: set  pindirs, 0         ;   glitch: release...
//  17: set  pins, 0
//  18: jmp  11                 ;   ...and wait for the real one
//  19: wait 0 pin 0            ; hold until it falls
//  20: set  pindirs, 0         ; release: high-Z
//  21: set  pins, 0
//  22: push noblock            ; "done"   .wrap (-> 0)
inline constexpr uint16_t PRESS_PROGRAM[] = {
    0x80A0,  //  0 pull block
    0x6020,  //  1 out x, 32
    0x002B,  //  2 jmp !x, 11
    0x20A0,  //  3 wait 1 pin 0
    0xB342,  //  4 mov y, y [19]
    0x00C7,  //  5 jmp pin, 7
    0x0003,  //  6 jmp 3
    0x2020,  //  7 wait 0 pin 0
    0xB342,  //  8 mov y, y [19]
    0x00C7,  //  9 jmp pin, 7
    0x0042,  // 10 jmp x--, 2
    0x20A0,  // 11 wait 1 pin 0
    0xE001,  // 12 set pins, 1
    0xE081,  // 13 set pindirs, 1
    0xB342,  // 14 mov y, y [19]
    0x00D3,  // 15 jmp pin, 19
    0xE080,  // 16 set pindirs, 0
    0xE000,  // 17 set pins, 0
    0x000B,  // 18 jmp 11
    0x2020,  // 19 wait 0 pin 0
    0xE080,  // 20 set pindirs, 0
    0xE000,  // 21 set pins, 0
    0x8000,  // 22 push noblock
};
inline constexpr uint8_t PRESS_WRAP_TARGET = 0;
inline constexpr uint8_t PRESS_WRAP = 22;
// OSR shifts RIGHT (out x, 32 takes the whole word), no autopull. jmp_pin = clock pin.
// Both programs together use 31 of a PIO block's 32 instruction slots.

// Convert a capture word's count to pulse width in 0.1 us units.
inline uint32_t capture_width_x10(uint32_t word, uint32_t sm_hz = SM_CLOCK_HZ) {
  const uint32_t count = 0x3FFFFFFFu - (word & 0x3FFFFFFFu);
  return static_cast<uint32_t>((static_cast<uint64_t>(2 * count + 3) * 10'000'000ull) / sm_hz);
}
inline bool capture_data(uint32_t word) { return (word >> 31) & 1u; }

}  // namespace jacuzzi_bus
