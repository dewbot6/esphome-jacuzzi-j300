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

// --- Press: drive the reply pin high for one button-scan slot -------------------------
// TX word = number of clock pulses to let pass before the one to drive (k-1 for scan
// position k, written right after short pulse 24 ends). Every pulse it counts must stay
// high for ~1 us (and every low must stay low ~1 us), so sub-microsecond glitches on the
// clock line are never counted. It drives from the target pulse's rising edge (~0.1 us),
// re-checks after ~1 us that it really is a pulse (else releases and waits for the real
// one), then holds for a fixed ~11.6 us from the rising edge: through the 7 us pulse AND the
// ~4 us low gap after it, ~0.6 us into pulse k+1. Then it releases (high-Z) and pushes one RX
// word ("done"). Timing the hold (not waiting for the falling edge) means a sub-microsecond
// dip on the clock can't cut the press short.
//
//
// The hold matches the real panel (Logic 8, captures 31/32): its reply rises 0.28 us after
// pulse k rises and falls 0.8 us after pulse k+1 rises (4.7-4.8 us after pulse k falls),
// 11.5 us in all. The wide pulses are 7.04 us high, 11.0 us apart in every capture.
// Releasing at the falling edge (the 2026-10-26/27 firmware) was ignored by the board, which
// evidently samples the reply after it drops the clock.
// SET_BASE = reply pin, SET_COUNT = 1.
//
// Two ways to wire the reply pin (yaml reply_buffer):
//   none:  GP4 -> J1-A pin 4 directly. Drive = pindirs 1 (high), release = pindirs 0 (high-Z).
//   diode: GP4 -> 5 V buffer (Adafruit Pixel Shifter) -> diode -> J1-A pin 4. The firmware
//          forces GP4's output enable on, so GP4 is always driven: low = released (the diode
//          blocks), high = drive. Only the `pins` latch matters, which every path sets back to 0.
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
//  17: set  pins, 0            ;   (latch low too: with reply_buffer: diode the pin's output
//  18: jmp  11                 ;   enable is forced on, so the latch alone sets the level)
//  19: set  y, 25              ; hold 26 x 8 cycles = 10.4 us more: release ~11.6 us after
//  20: jmp  y--, 20 [7]        ;   the rising edge (~4.6 us after the fall), like the panel
//  21: set  pindirs, 0         ; release: high-Z (or, with OE forced on, low next cycle)
//  22: set  pins, 0
//  23: push noblock            ; "done"   .wrap (-> 0)
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
    0xE059,  // 19 set y, 25
    0x0794,  // 20 jmp y--, 20 [7]
    0xE080,  // 21 set pindirs, 0
    0xE000,  // 22 set pins, 0
    0x8000,  // 23 push noblock
};
inline constexpr uint8_t PRESS_WRAP_TARGET = 0;
inline constexpr uint8_t PRESS_WRAP = 23;
// OSR shifts RIGHT (out x, 32 takes the whole word), no autopull. jmp_pin = clock pin.
// Both programs together use all 32 of a PIO block's instruction slots.

// Convert a capture word's count to pulse width in 0.1 us units.
inline uint32_t capture_width_x10(uint32_t word, uint32_t sm_hz = SM_CLOCK_HZ) {
  const uint32_t count = 0x3FFFFFFFu - (word & 0x3FFFFFFFu);
  return static_cast<uint32_t>((static_cast<uint64_t>(2 * count + 3) * 10'000'000ull) / sm_hz);
}
inline bool capture_data(uint32_t word) { return (word >> 31) & 1u; }

}  // namespace jacuzzi_bus
