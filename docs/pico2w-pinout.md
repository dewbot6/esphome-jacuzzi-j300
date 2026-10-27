# Raspberry Pi Pico 2 W ↔ J1-A (read + control)

> For the hands-on, step-by-step version (parts, building the tap, flashing, tests), see
> **[SETUP.md](SETUP.md)**. This page is the technical reference.

Planned hardware for reading the panel bus directly and pressing buttons. Uses the
J1-A numbering from [j1a-pinout.md](j1a-pinout.md). The Pico 2 W runs ESPHome
(`rp2` platform, `board: rpipico2w`).

| Signal | J1-A pin | Tap splitter | Pico 2 W GPIO | Pico physical pin | Direction |
|---|---|---|---|---|---|
| Clock | 6 | S3 bottom | GP2 | 4 | in |
| Data | 5 | S3 top | GP3 | 5 | in |
| Button reply | 4 | S2 bottom | GP4 (through 470 Ω, or direct at `reply_drive_strength: 2mA`) | 6 | out, only while pressing |
| Ground | 8 | S4 bottom | GND | 3 (any GND) | — |
| +5 V | 7 | S4 top | VSYS | 39 | power in |
| Listen-only | 1 | S1 top | GP5 | 7 | in (never driven) |
| Listen-only | 2 | S1 bottom | GP6 | 9 | in (never driven) |
| Listen-only | 3 | S2 top | GP7 | 10 | in (never driven); measure first, see below |

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

## Listen-only inputs (J1-A pins 1–3)

Pins 1–3 are still unidentified (survey: pins 1 and 2 at a steady 5 V, pin 3 at 0 V). The
firmware watches them as high-impedance inputs with no pulls, and never drives them. Each one
reports its **level** (at most once a second) and a cumulative **edge count** (every 10 s;
the log notes any edges). Look for changes during presses, pump/heater changes and the
filter cycle.

**Pin 3 caution (E9):** on early RP2350 silicon, an input on a line that is only *weakly*
pulled low can drift up to ~2 V. Before connecting GP7, measure J1-A pin 3 to GND with the
breaker off. **Under ~8 kΩ** is safe. Otherwise leave it disconnected. (Pins 1 and 2 idle high
and aren't affected.) The boot log prints the chip's silicon revision field.

## Bring-up order

1. First flash over USB on the bench, **not** connected to the tub. Updates after that go over Wi-Fi.
2. Breaker off: measure J1-A pin 4 to GND and to +5 V, board side and panel side
   (expect a pull-down on the board), and pin 3 to GND (for the GP7 listen-only input).
3. Connect clock, data, ground, +5 V and the listen-only pins 1–2 (and 3 if it measured OK). Verify read-only decoding against the
   Logic 8 / Waveshare. Check the panel display is unaffected.
4. Connect GP4 (via 470 Ω). First live press: a single **+**, which only shows the set point.
   Confirm with the Logic 8 that the injected bit lands in scan position 4.
5. Then Jets 2 on/off, then a real set-point change.

Button scan positions (from `captures/31-j1a-buttons.sal`): 2 = Jets 2, 3 = −, 4 = +, 7 = Jets 1.
The output is driven high for the whole of that wide clock pulse, for ~3 frames (300 ms).

## Firmware

`jacuzzi_panel.yaml` + `components/jacuzzi_panel/` (ESPHome, `rp2` / `rpipico2w`).

All bus timing is done in **PIO** hardware (`bus_pio.h`). No interrupt sits in the timing path.
(The first version used a core-1 GPIO interrupt. On the tub it decoded only ~1 frame in 10,
because 2 µs clock pulses outran the interrupt, and its presses never registered.)

- **Capture state machine:** on every clock rise it samples DATA and counts how long the clock
  stays high. It pushes one word per pulse into an 8-deep FIFO. It cannot miss an edge.
- **Core 1** (a tight loop in `loop1`, away from Wi-Fi on core 0) drains the FIFO into
  `bus_assembler.h`. That classifies pulses (glitch < 1 µs, short ≤ 4 µs, wide 5–11 µs), builds
  24-bit frames, and decides when to arm a press. Values publish after two identical frames.
- **Press state machine:** core 1 arms it right after short pulse 24 of a frame, with
  "let k−1 pulses pass". It counts only pulses that stay high ≥ 1 µs (glitch-proof). It drives
  GP4 high from the target pulse's **rising edge (~0.1 µs)**, re-checks after 1 µs that it was a
  real pulse, holds for a fixed **11.6 µs** from the edge (through the 7 µs pulse and the ~4 µs gap
  after it, like the real panel: see docs/protocol.md), then releases GP4 to high-Z and reports
  "done". With `reply_buffer: diode` (GP4 → Pixel Shifter → BAT85 diode → pin 4, SETUP.md
  step 8) the firmware forces GP4's output enable on, so GP4 sits low between presses and the
  diode keeps pin 4 free; the same program drives it high for the 11.6 µs.
- **Arming rules:** only on a normal frame (exactly 24 short pulses), only if the 24 pulses spanned
  370–396 µs (a wrong count can't have the right span), only if core 1 isn't running behind. A
  press holds for 3 frames (max 5 drive windows), with 400 ms between queued presses. If the frame
  goes wrong after arming, core 1 resets the press SM, so it never waits into the next frame.
- **Control:** the **Control Enabled** switch is forced OFF at boot (`restore_mode: ALWAYS_OFF`
  is enforced by the schema). It refuses to turn on until valid frames are arriving, and it
  turns itself off if the bus is lost for 2 s. Buttons: Temp Up (scan pos 4), Temp Down (3),
  Jets 1 Button (7), Jets 2 Button (2).
- **Boot self-check:** the hand-encoded PIO words are compared with the SDK's `pio_encode_*()`.
  On any mismatch the component fails: no reading, no control.
- Stats every 30 s: `frames=… bad=… refusals=… drives=… aborts=… fifo_overflows=…`. Expect
  `frames` to rise by ~330 per 30 s.

### Testing without hardware

`tools/test_bus_sim.cpp` runs the **exact PIO instruction words** in a cycle-by-cycle PIO simulator
(20 MHz) against a J1-A Logic 8 recording. It models core 1 polling the FIFOs and feeding the same
assembler:

```
c++ -std=c++17 -O2 -I components/jacuzzi_panel tools/test_bus_sim.cpp -o /tmp/bussim
/tmp/bussim captures/31-j1a-buttons/digital.csv        # read-only: frames vs recording
/tmp/bussim captures/31-j1a-buttons/digital.csv 4      # + a press of scan pos 4 every 2 s
DROP_PCT=5 GLITCH_PER_S=500 POLL_NS=20000 /tmp/bussim captures/33-j1a-light/digital.csv 7
```

Every drive window is checked against the real wide pulse *k* in the recording. Results
2026-10-26:
- 100 % of frames decoded on all four captures, all matching the recording.
- Every press placed correctly for scan positions 2/3/4/7 on every capture.
- Drive starts 115–140 ns after the clock rises.
- Still zero misplaced with 5 % dropped FIFO words, 20 µs polling gaps, or 2000 clock glitches/s.
  The only oddity under heavy combined noise: a glitch just before the target pulse can start the
  drive up to ~1 µs early; it still covers the correct pulse (reported as `early_but_correct`).

`tools/check_pio_encoding.cpp` checks the PIO words against the SDK encoders on a PC (build line
in the file). Export `digital.csv` from the `.sal` in Saleae Logic 2 before running the simulator.

**Hardware status:** the interrupt version read the tub (badly) and pressed nothing. The PIO
version has not run on the tub yet. Also still unverified: whether the PIC accepts a 3.3 V high
on the reply line, and J1-A pin 4's pull-down value.
