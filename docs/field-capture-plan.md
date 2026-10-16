# Field capture plan — Saleae Logic 8 on the RJ45 diagnostic port

Goal: start over on the physical layer. Record every RJ45 pin raw (analog + digital)
and let the waveforms say what the interface is — UART/RS-485, RS-232, or a
clocked display bus like the one in
<https://forums.raspberrypi.com/viewtopic.php?t=175399> (~40 kHz clock + data,
21 bits/frame, ~30 frames/s, 7-segment bitmap).

The Logic 8 is input-only, so nothing here can put traffic on the bus.

## Bring

- Laptop (charged — run it **on battery** at the tub to avoid a ground loop through the charger)
- Saleae Logic 8 + USB cable + lead harness + micro-hook clips
- RJ45 screw-terminal breakout, or a patch cable you're willing to cut
- RJ45 pass-through/splitter (in case the port also feeds the topside panel)
- Multimeter

## Wiring

| Logic 8 | RJ45 pin | Expected (from May notes — verify) |
|---------|----------|------------------------------------|
| CH0     | 1        | RS-232? / maybe +5 V (forum J-LXL guess) |
| CH1     | 2        | RS-232? / maybe GND |
| CH2     | 3        | GND |
| CH3     | 4        | "RS-485 A" |
| CH4     | 5        | "RS-485 B" |
| CH5     | 6        | ? |
| CH6     | 7        | ? |
| CH7     | 8        | +12 V (analog clips at ±10 V; inputs are ±25 V protected) |
| GND     | 3        | GND |

Channel N = pin N+1, so every pin is recorded, GND included (sanity check that it reads 0 V).
Before clipping on, confirm pin 3 is ground and pin 8 is ≤ 12 V with the multimeter.

## Claude setup

1. Open Saleae Logic (2.4.48) — its MCP server listens on `127.0.0.1:10530`.
2. Plug in the Logic 8.
3. Start a **new** Claude Code session in `~/GitHub/esphome-jacuzzi`. The `saleae` MCP
   server is registered at user scope and connects at session start.

Logic 8 notes learned from a dry run on the simulated device:
- Digital threshold is fixed — don't pass `digitalThresholdVolts`.
- All 8 digital + 8 analog: use **25 MS/s digital + 625 kS/s analog**.
- `captureId` is a number.

## Capture sequence

Save each capture as `captures/NN-<label>.sal`.

1. **01-survey** — 2 s, all channels analog + digital, nobody touching the panel.
   Answers: which pins are power/ground, which idle high, which toggle, and at what levels.
2. **02-idle-long** — 10 s digital-only on the active pins at max rate. Gives frame
   period, burst structure, and clock-vs-UART timing.
3. **Labeled button captures** — ~6 s each, one press near the 2 s mark. Note what the
   display showed before and after.
   - 10-temp-up, 11-temp-down
   - 12-jets1, 13-jets2
   - 14-light
   - 15-mode/other buttons
4. **Heater state** — if you can catch the heater turning on/off, grab a capture each way.

Diffing the labeled captures against idle tells us which bits mean what.
