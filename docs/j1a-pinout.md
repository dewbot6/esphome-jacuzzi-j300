# J1-A — topside panel connector

J1-A is a shrouded 2×4, 0.1″ (2.54 mm) male header on the main board, just left of
the RJ45. The topside panel cable plugs into it with a 2×4 crimp housing.

## Numbering (shared convention)

Board facing you, `PN: 4PB00931D` text upright (U5 to the left of J1-A, RJ45 to the right):

```
              board top edge
          ┌────┬────┬────┬────┐
  top row │ 1  │ 3  │ 5  │ 7  │
          ├────┼────┼────┼────┤      ← RJ45 is off to this side
  bot row │ 2  │ 4  │ 6  │ 8  │
          └────┴────┴────┴────┘
   column:  C1   C2   C3   C4
              toward U5 / PIC
```

- Pin = 2·column − 1 (top row) or 2·column (bottom row).
- Tap splitter **S1–S4 = column C1–C4** (each 1×2 female plug spans one column).
- **Logic 8 channel = pin − 1** (CH0 = pin 1 … CH7 = pin 8). Ground clip on RJ45 pin 3.
- Mark pin 1 on the board and the matching corner of the panel plug before unplugging.

## Findings

Fill in from the continuity check (breaker off, panel unplugged) and the captures.

| J1-A pin | Splitter | Wire color | Continuity to RJ45 pin | Signal |
|---|---|---|---|---|
| 1 | S1 top | | | 5.0 V steady, idle high (button reply?) |
| 2 | S1 bottom | | | 5.0 V steady, idle high (button reply?) |
| 3 | S2 top | | | 0 V steady, idle low |
| 4 | S2 bottom | | | 0 V steady, idle low |
| 5 | S3 top | | | **DATA**: 24 bits per frame, 1 bit per clock |
| 6 | S3 bottom | | | **CLOCK** (same signal as RJ45 pin 4) |
| 7 | S4 top | | | **+5 V** supply (5.10 V, clean) |
| 8 | S4 bottom | | 3 (GND) | **Ground** |

Survey 2026-10-20 (`captures/30-j1a-survey.sal`): reading DATA at each clock pulse gives
24 bits whose bits 2–24 equal the timing-decoded slots 1–23 exactly; bit 1 (the hidden
first bit) is 1. No strobe line on J1-A. Pins 1–4 are static during the 7-pulse button scan
with no buttons pressed.

Originally expected among them: ground (= RJ45 3), clock (= RJ45 4), strobe (= RJ45 5), the
display data line, the button reply line, and panel power.
