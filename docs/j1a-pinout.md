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
| 1 | S1 top | | | |
| 2 | S1 bottom | | | |
| 3 | S2 top | | | |
| 4 | S2 bottom | | | |
| 5 | S3 top | | | |
| 6 | S3 bottom | | | |
| 7 | S4 top | | | |
| 8 | S4 bottom | | | |

Expected among them: ground (= RJ45 3), clock (= RJ45 4), strobe (= RJ45 5), the
display data line, the button reply line, and panel power.
