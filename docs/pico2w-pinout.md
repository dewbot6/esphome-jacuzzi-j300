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

- **Read:** a clock interrupt on **core 1** (away from Wi-Fi on core 0) samples DATA on
  every rising edge. `panel_decoder.h` turns edges into 24-bit frames (bit 1 = MSB). Values
  publish after two identical frames. Entities: Water Temperature, Set Temperature, Display,
  Heater, Jets 1, Jets 2, Status Slot 5, Panel Bus, Raw Frame.
- **Control:** the **Control Enabled** switch is forced OFF at boot (`restore_mode: ALWAYS_OFF`
  is enforced by the schema). It refuses to turn on until valid frames are arriving, and it
  turns itself off if the bus is lost for 2 s. Buttons: Temp Up (scan pos 4), Temp Down (3),
  Jets 1 Button (7), Jets 2 Button (2). A press is queued, then driven for 3 frames (max 5 drive
  windows), with 400 ms between presses.
- **Press placement:** GP4 is driven only from the rising edge of wide pulse *k* of a normal
  frame (exactly 24 short pulses before it), and only if that edge arrives within ±3 µs of
  `rise(short 24) + 17 + 11·(k−1)` µs. It is released on the next edge. Any missed edge
  means no press. It never means a press in the wrong slot.

### Testing without hardware

`tools/test_panel_decoder.cpp` replays a J1-A Logic 8 capture through the same decoder:

```
c++ -std=c++17 -O2 -I components/jacuzzi_panel tools/test_panel_decoder.cpp -o /tmp/tpd
/tmp/tpd captures/31-j1a-buttons/digital.csv            # read-only: prints frame changes
/tmp/tpd captures/31-j1a-buttons/digital.csv 4 1.0      # arm a press at scan pos 4 at t=1 s
DROP_PCT=5 /tmp/tpd captures/31-j1a-buttons/digital.csv 4 1.0   # with 5% of edges lost
```

Each drive window is checked against the real (undropped) wide pulse *k*. Results
2026-10-20: frames match `tools/j1a_buttons.py` exactly. Every press window was correctly
placed for positions 2/3/4/7 on three captures, including at 1/5/10% edge loss.
(Export `digital.csv` from the `.sal` in Saleae Logic 2 first.)

**Not yet tested on hardware:** the Pico hasn't arrived. Also unverified: whether the PIC
accepts a 3.3 V high on the reply line, and J1-A pin 4's pull-down value.
