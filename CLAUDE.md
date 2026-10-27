# CLAUDE.md: Jacuzzi J355 → Home Assistant

You are probably helping someone at the hot tub wire up and flash this, following
**[docs/SETUP.md](docs/SETUP.md)**. Walk them through it one step at a time. Have them
confirm each check before moving on, and don't skip the measurement steps.

## What this is

- Jacuzzi J355, J-300 series main board (PCB `6600-087 REV D`, PN `4PB00931D`, PIC firmware `REV 3.61A`).
- The main board bit-bangs the topside panel over the 8-pin header **J1-A**: every ~92 ms,
  24 data bits (heater, Jets 1/2, three BCD display digits), then a 7-pulse button scan where
  the panel raises a reply line for the pressed button. Spec: [docs/protocol.md](docs/protocol.md).
- **Main method:** Raspberry Pi Pico 2 W on J1-A: `jacuzzi_panel.yaml`, `components/jacuzzi_panel/`.
  Wiring and bring-up: [docs/pico2w-pinout.md](docs/pico2w-pinout.md), [docs/j1a-pinout.md](docs/j1a-pinout.md).
- **Backup method:** read-only Waveshare ESP32 on the RJ45 port, in [`rj45/`](rj45/README.md). It is
  deployed and working, so leave it alone unless asked.
- Status (2026-10-27): **reading works on the tub** (PIO capture, ~330 frames per 30 s).
  **Control not yet working.** Presses are sent (`drives` +3 per press) but the board ignores
  them: first releasing at the scan pulse's falling edge (2 mA), then with an 11.6 µs hold matching
  the real panel's timing within ~0.2 µs (docs/protocol.md "Reply timing") at 2, 4 and 8 mA. Not
  even the first-press "show set point" happens, while a hand press logged fine. Suspect the
  electrical side (3.3 V high too low for the board's input, or the wiring); measure before changing firmware.

## Safety rules (non-negotiable)

1. **Never drive CLOCK (J1-A 6) or DATA (J1-A 5)**, and never transmit on the RJ45 lines.
   Doing that in May caused a **C96 error and a stuck recirc pump**.
2. The only output is the **reply line (J1-A 4 → GP4**, through 470 Ω if fitted; without it,
   `reply_drive_strength` stays at `2mA`, never above `8mA`), driven only during a scan
   pulse, only while **Control Enabled** is on. Don't weaken this:
   - keep `restore_mode: ALWAYS_OFF` on the control switch (the schema enforces it);
   - keep the bus-OK interlock, the 24-pulse + span checks in `bus_assembler.h`, and the
     glitch checks in the PIO press program (`bus_pio.h`);
   - don't add "boost" circuits or change the reply pin to push-pull outside the scan window.
3. Wiring changes happen with the tub **off at the main house panel**: 240 V is inside the bay.
4. **No USB to the Pico while it's wired to the tub** (it's powered from J1-A pin 7). Use Wi-Fi
   for logs/OTA: `esphome logs jacuzzi_panel.yaml`, `esphome run jacuzzi_panel.yaml`.
5. **GP7 (J1-A pin 3) only if pin 3 → GND measured under ~8 kΩ** (RP2350 erratum E9).
6. If the panel shows an error code or behaves oddly: tub off, remove the tap, plug the panel
   straight back into J1-A (see "Undo everything" in SETUP.md), then report.

## Commands

```
python3 -m venv ~/esphome-venv && ~/esphome-venv/bin/pip install esphome
~/esphome-venv/bin/esphome config  jacuzzi_panel.yaml     # validate
~/esphome-venv/bin/esphome run     jacuzzi_panel.yaml     # build + flash (BOOTSEL over USB first time, then OTA)
~/esphome-venv/bin/esphome logs    jacuzzi_panel.yaml     # live logs over Wi-Fi
```

`secrets.yaml` (git-ignored) is created from `secrets.example.yaml`. The Pico uses
`panel_api_encryption_key`. Never print secret values into chat or commit them.

Offline test (no hardware needed). It runs the exact PIO words in a simulator against a recording:
```
c++ -std=c++17 -O2 -I components/jacuzzi_panel tools/test_bus_sim.cpp -o /tmp/bussim
/tmp/bussim captures/31-j1a-buttons/digital.csv 4     # must report decoded 100%, misplaced=0
```
(Export `digital.csv` from the `.sal` with Saleae Logic 2 first.) Re-run it, and
`tools/check_pio_encoding.cpp`, after any change to `bus_pio.h` or `bus_assembler.h`.

## What to expect in the logs

- Boot: `RP2350 chip revision field: N`, `Listen-only: J1-A pin 1 on GP5` …
- Bus up: `frame 1000…  heater=0 jets1=0 jets2=0 slot5=0` (bit 1 is always 1), then `water 94`.
- After + / −: `set point NN` about 5 s after the display returns to water temperature.
- Control: `Control ENABLED`, `Pressing scan pos 4` (+), 3 (−), 7 (Jets 1), 2 (Jets 2).
- Boot: `PIO: capture pioN smN, press pioN smN, SM clock 20000000 Hz`. A `PIO setup FAILED` or
  `program word … != SDK` line means the component refused to start; report it.
- Every 30 s: `frames=… bad=… refusals=… drives=… aborts=… fifo_overflows=…`. **`frames` should
  rise by ~330 per 30 s** (one frame every ~92 ms). Much less means the bus isn't being read
  cleanly; check the wiring before testing control. `refusals` = frames skipped for a press
  because their timing looked wrong (safe). `fifo_overflows` should stay 0.

## Open questions (record findings in docs/j1a-pinout.md)

- Does the PIC accept the Pico's **3.3 V** high on the reply line? If the 11.6 µs hold still
  isn't taken at 8 mA, that's the likely reason.
- `bad` frames: 0 with only the heater on, then ~2-5% from the moment the jets started
  (2026-10-27 13:09), Control on or off. Likely pump-motor noise. Bad frames are dropped and
  never armed, so it's safe.
- J1-A **pin 4** pull-down value; **pins 1–3** function; **bit 6 / slot 5** meaning.

## Conventions

- J1-A numbering is board-anchored zigzag: odd = top row, column 1 nearest U5 (docs/j1a-pinout.md).
- Data bits are numbered 1–24; the firmware's "slot k" = bit k+1 (the RJ45 decode can't see bit 1).
- Commit messages end with a `Co-Authored-By` line for Claude. Don't push without asking.
