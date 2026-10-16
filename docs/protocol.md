# J-300 RJ45 port — decoded protocol

Board: PCB `6600-087 REV D`, PN `4PB00931D`, PIC (DIP-28, `REV 3.61A`) at 4 MHz.
Captured 2026-10-16 with a Saleae Logic 8 (`captures/`, decoder `tools/slots.py`).

It is **not RS-485 or a UART**. There is no transceiver on the board; the RJ45 lines
go straight to the PIC. The display data line itself is not on the RJ45, but the
PIC's bit-banged clock leaks every data bit through its timing.

## Pins (RJ45, pin 3 = reference)

| Pin | Signal |
|-----|--------|
| 1   | weak pull-up (~3–4 V, mains ripple), static |
| 2   | 0 V, static |
| 3   | ground reference (sits ~0.5 V above PIC ground — lows read −0.5 V) |
| 4   | **CLK** — 5 V push-pull |
| 5   | **STROBE** — active low, slow RC edges |
| 6   | weak, static (picks up switching noise) |
| 7   | 0 V, static |
| 8   | 0 V, static (**not** +12 V as the May notes said) |

## Framing

Every 100.00 ms:

- STROBE (pin 5) falls; ~11 µs later CLK (pin 4) sends **24 short pulses** (2 µs wide).
- STROBE rises, then CLK sends **7 wide pulses** (7 µs wide, 11 µs period).

Once per second the frame is preceded by a 15-pulse **header** (strobe→first clock
13 µs). The header was constant in every capture.

## Bits

The gap between consecutive short clock rising edges is **15 µs = 1** or **17 µs = 0**
(a data-dependent branch on the 1 MIPS PIC). 24 pulses give 23 visible slots; the
first bit has no preceding gap and is not visible.

| Slot  | Meaning | How confirmed |
|-------|---------|---------------|
| 4     | Heater on (= red dot on panel) | set point above/below water temp |
| 5     | Always 1 — not shown on panel; suspect circulation pump | never changed |
| 7     | Jets 1 on | Jets 1 on/off |
| 11    | Jets 2 on | Jets 2 on/off |
| 12–15 | Display hundreds digit, BCD | 100/101 shown |
| 16–19 | Display tens digit, BCD | |
| 20–23 | Display units digit, BCD | 93–101 sweep |
| other | 0 in every capture | |

Display shows water temperature; after a +/− press it shows the set point
(the first press only shows it), then reverts **3.0 s** after the last press.
The heater flag updates on the 1 s tick.

**Not on this bus:** Light on/off and light color (footprint-bulb button).
Their only trace is sub-µs switching noise on pins 4/6.

## ESP32 hookup (Waveshare ESP32-S3-RS485-CAN)

The isolated RS-485 receiver works as a level shifter, wired **A+ → RJ45 pin 3,
B− → RJ45 pin 4**, RS-485 GND → pin 3, 120R jumper off, EN (GPIO21) held low.
(A+ on pin 4 does *not* work: idle A−B ≈ 0 V sits in the receiver's fail-safe
"high" zone, so pulses barely register.) The output on GPIO18 is an inverted clock;
`components/jacuzzi_monitor` times it with RMT RX and auto-detects polarity.

With the receiver attached, **a clock pulse that follows a 15 µs ('1') gap is lost**,
probably because the PIC drives the clock only weakly at that point. The bits survive:
between two observed pulses j < m the gap is 17 + 15·n µs, with slots j..m−2 = 1 and
slot m−1 = 0. Pulse 1 is also missing (the hidden first bit is 1). When slot 23
is 1 (odd units digit) pulse 24 **and all 7 wide pulses** are lost too, so those
frames are decoded forward from pulse 2 with the trailing missing pulses as 1s. The topside panel
keeps working normally with the receiver attached.

## Decoder notes

- Ignore pulses < 1 µs on CLK: pump/heater relays and the light driver inject
  0.1–1 µs glitches.
- Stretched gaps (~40 µs) occasionally appear inside a frame (PIC interrupt) —
  treat that slot as unknown and keep the previous frame's value.
- The port is receive-only for us. Never drive these lines (C96 error and stuck
  recirc pump history).
