# Raspberry Pi Pico 2 W ↔ J1-A (read + control)

Planned hardware for reading the panel bus directly and pressing buttons. Uses the
J1-A numbering from [j1a-pinout.md](j1a-pinout.md). The Pico 2 W runs ESPHome
(`rp2` platform, `board: rpipico2w`).

| Signal | J1-A pin | Tap splitter | Pico 2 W GPIO | Pico physical pin | Direction |
|---|---|---|---|---|---|
| Clock | 6 | S3 bottom | GP2 | 4 | in |
| Data | 5 | S3 top | GP3 | 5 | in |
| Button reply | 4 | S2 bottom | GP4 (through 470 Ω) | 6 | out, only while pressing |
| Ground | 8 | S4 bottom | GND | 3 (any GND) | — |
| +5 V | 7 | S4 top | VSYS | 39 | power in |
| — | 1, 2, 3 | S1 top/bottom, S2 top | not connected | — | — |

## Why

- GP2/GP3 are adjacent so one PIO program can sample clock and data together; GP4 is
  next to them. All are 5 V-tolerant digital pins. GP26–28 (ADC) are **not** 5 V-tolerant.
- The RP2350's 5 V tolerance only holds while it is powered. The bus is live 24/7, so
  the Pico is powered from J1-A pin 7 via VSYS. Then it is always up when the
  signals are.
- 470 Ω in the reply line limits current if the panel ever drives pin 4 low while
  we drive it high. It can be clamped directly in the screw terminals, no solder needed.
- The RP2350 erratum E9 (early chips; fixed in A4) can let an undriven input pad sit near
  2 V, which on the reply line would look like a held button. The firmware keeps GP4's
  input buffer disabled except while pressing.

## Bring-up order

1. First flash over USB on the bench, **not** connected to the tub. Updates after that go over Wi-Fi.
2. Breaker off: measure J1-A pin 4 to GND and to +5 V, board side and panel side
   (expect a pull-down on the board).
3. Connect clock, data, ground and +5 V only. Verify read-only decoding against the
   Logic 8 / Waveshare. Check the panel display is unaffected.
4. Connect GP4 (via 470 Ω). First live press: a single **+**, which only shows the set point.
   Confirm with the Logic 8 that the injected bit lands in scan position 4.
5. Then Jets 2 on/off, then a real set-point change.

Button scan positions (from `captures/31-j1a-buttons.sal`): 2 = Jets 2, 3 = −, 4 = +, 7 = Jets 1.
The output is driven high for the whole of that wide clock pulse, for ~3 frames (300 ms).
