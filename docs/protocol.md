# J-300 topside panel bus (J1-A): protocol

Board: PCB `6600-087 REV D`, PN `4PB00931D`. A PIC (DIP-28, firmware `REV 3.61A`, 4 MHz)
bit-bangs the topside panel over the 8-pin header **J1-A**. Decoded from Saleae Logic 8
captures on J1-A (`captures/30`–`33`). Pin numbering: [j1a-pinout.md](j1a-pinout.md).
Firmware: `components/jacuzzi_panel/` (Pico 2 W). The same bus can also be read
indirectly from the RJ45 port; see [../rj45/](../rj45/README.md) (backup method).

## Signals

| J1-A pin | Signal | Notes |
|---|---|---|
| 5 | **DATA** | main board → panel, 5 V push-pull, valid ~3 µs before each clock rise |
| 6 | **CLOCK** | main board → panel, 5 V push-pull, idle low |
| 4 | **BUTTON REPLY** | panel → main board, high during the pressed button's scan pulse |
| 7 | +5 V | 5.10 V, panel supply |
| 8 | Ground | |
| 1, 2 | unidentified | steady 5 V so far |
| 3 | unidentified | steady 0 V so far |

No strobe or latch line on J1-A. The gap between frames resets the panel.

## Frame (every 100 ms)

```
CLOCK  _|‾|__|‾|__ … 24 short pulses … _|‾|___|‾‾‾‾|__|‾‾‾‾|__ … 7 wide pulses
       2 µs high, 15–17 µs apart          7 µs high, 11 µs apart
DATA   bit 1 … bit 24 (sampled on the rising edge)
REPLY                                       high in the pressed button's slot
```

1. **24 data bits** to the panel, bit 1 first. The panel latches DATA on each rising edge.
2. **7-pulse button scan.** The panel raises REPLY during the wide pulse for a held button.

Once per second a **header block** (8 short + 7 wide pulses, data `00111001` then 1s)
precedes the frame. It has been identical in every capture; its purpose is unknown.

## Data bits

| Bit | Meaning |
|---|---|
| 1 | always 1 |
| 5 | **heater on** (the panel's red dot) |
| 6 | unknown; was 1 until 2026-10-16, 0 since ("slot 5" in the firmware) |
| 8 | **Jets 1 on** |
| 12 | **Jets 2 on** |
| 13–16 | display **hundreds** digit, BCD, MSB first |
| 17–20 | display **tens** digit, BCD |
| 21–24 | display **units** digit, BCD |
| others | 0 in every capture |

The firmware names bits by "slot" = bit − 1 (slot 1–23). That is the numbering the RJ45
timing decode uses, since it can't see bit 1.

## Buttons (scan positions)

| Position | Button |
|---|---|
| 2 | Jets 2 |
| 3 | − |
| 4 | + |
| 7 | Jets 1 |
| 1, 5, 6 | not used on this panel |

The board polls every 100 ms, so a press must last about 100 ms or longer. **Light and light
color** are on a separate ribbon cable from the panel and never appear on this bus.

## Display behaviour

- The display shows **water temperature**.
- After + or −, it shows the **set point**. The first press only shows it, without
  changing it. The display reverts to water temperature **~3 s** (seen 3.0–3.9 s) after
  the last press.
- The heater bit follows the set point vs. water temperature, updating on a ~1 s tick.

## Electrical notes

- Logic 5 V. Relay switching and the light driver inject sub-µs glitches; ignore pulses < 1 µs.
- With an RS-485 receiver loading the clock (RJ45 method), some clock pulses collapse;
  on J1-A with a high-impedance input every pulse is clean.
- **Never drive CLOCK or DATA.** Transmitting on the bus once caused a C96 error and a stuck
  recirc pump. Only REPLY is ever driven, and only during a scan pulse.
