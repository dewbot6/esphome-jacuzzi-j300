# Jacuzzi J355 → Home Assistant

ESPHome firmware for a Jacuzzi J355 hot tub (J-300 series main board, PCB `6600-087 REV D`,
firmware `REV 3.61A`). A **Raspberry Pi Pico 2 W** taps the topside panel connector
**J1-A**, reads the panel bus directly, and can press panel buttons from Home Assistant,
but only while its **Control Enabled** switch is on.

> **Status:** compiled and tested offline against Logic 8 captures. **Not yet run on
> hardware.** A working read-only backup on the RJ45 port lives in [`rj45/`](rj45/README.md).

**➜ To build, wire and flash it, follow [docs/SETUP.md](docs/SETUP.md)** (step-by-step, with a
[wiring diagram](docs/wiring.svg)). Using Claude Code? [`CLAUDE.md`](CLAUDE.md) gives it the context and safety rules.

## What shows up in Home Assistant

| Entity | What it is |
|---|---|
| **Water Temperature** | °F, as shown on the topside display |
| **Set Temperature** | °F. The tub only shows it after + or −, so it updates a few seconds after a press. Unknown after a reboot until the next press. |
| **Heater** | On when the heater is running (the red dot on the panel) |
| **Jets 1**, **Jets 2** | On/off |
| **Control Enabled** | Off at every boot. Must be on for the buttons below to do anything. |
| **Temp Up**, **Temp Down**, **Jets 1 Button**, **Jets 2 Button** | Press the panel button |
| Display, Status Slot 5, Panel Bus, Raw Frame | Diagnostics |
| J1-A Pin 1/2/3 (+ Edges) | Diagnostics: listen-only watch on the unidentified pins |

**Not available:** the light (on/off and color). The lights have their own cable from the
topside panel and never appear on this bus.

## Hardware

- Raspberry Pi Pico 2 W (headers pre-soldered) on a screw-terminal breakout
- A tap on J1-A that leaves the panel connected (2-pin PC front-panel Y-splitters + a 2×4
  male-male header block for the panel plug)
- 470 Ω resistor in the reply line (clamped in the screw terminal, no solder)

Step-by-step build and bring-up: **[docs/SETUP.md](docs/SETUP.md)**. Pin-level details:
[docs/pico2w-pinout.md](docs/pico2w-pinout.md).
Connector numbering: [docs/j1a-pinout.md](docs/j1a-pinout.md).

## Flashing

1. Install ESPHome (`pip install esphome`) or use the ESPHome add-on / Builder.
2. `cp secrets.example.yaml secrets.yaml` and fill in Wi-Fi, OTA password and the API
   encryption keys (generate each with `openssl rand -base64 32`).
3. First flash over USB, on the bench, **not connected to the tub**:
   `esphome run jacuzzi_panel.yaml --device /dev/cu.usbmodem…`
4. Later updates over Wi-Fi: `esphome run jacuzzi_panel.yaml`

In Home Assistant, **Settings → Devices & services** discovers "Jacuzzi J355 Panel". Paste
`panel_api_encryption_key` from `secrets.yaml`.

## How it works

Every 100 ms the main board clocks 24 data bits to the panel: the heater, both jets and three
BCD display digits. Then it runs a 7-pulse button scan, during which the panel raises the reply
line in the pressed button's slot. The Pico's PIO hardware captures every clock pulse and
DATA bit with no software in the timing path. Its second core assembles the frames, away from
Wi-Fi. To press a button, a second PIO state machine drives the reply line from the exact rising
edge of that button's scan pulse to its falling edge, ignoring sub-microsecond glitches. It
only arms on a frame whose pulse count and timing check out. Otherwise it doesn't press at all.

Full protocol: **[docs/protocol.md](docs/protocol.md)**.

## Repository layout

| Path | Contents |
|---|---|
| `jacuzzi_panel.yaml` | ESPHome config for the Pico 2 W |
| `components/jacuzzi_panel/` | The firmware component. `bus_pio.h` holds the PIO programs; `bus_assembler.h` is platform-free frame logic. |
| `docs/protocol.md`, `docs/j1a-pinout.md`, `docs/pico2w-pinout.md` | Protocol, connector numbering, Pico wiring and bring-up |
| `docs/*.webp` | Board photos |
| `captures/*.sal` | Logic 8 captures on J1-A (open in Saleae Logic 2) |
| `tools/` | Capture and test tools (below) |
| `rj45/` | **Backup method:** read-only Waveshare ESP32 on the RJ45 port (working, deployed) |
| `docs/history/` | Earlier notes, kept for reference. Some of their conclusions turned out wrong. |

### Tools

- `tools/saleae.py`: drives a Logic 8 through Saleae Logic 2's built-in MCP server
  (127.0.0.1:10530) to record and export captures into `captures/`.
- `tools/j1a_buttons.py <digital.csv>`: decodes data bits and button-scan bits from a J1-A capture.
- `tools/test_bus_sim.cpp`: runs the firmware's exact PIO programs in a PIO simulator against
  a J1-A recording, including simulated presses, dropped words and glitches (see docs/pico2w-pinout.md).
- `tools/check_pio_encoding.cpp`: checks the hand-encoded PIO words against the Pico SDK's encoders.

Python tools need `numpy` (`pip install -r tools/requirements.txt`). Export a `.sal` to
`digital.csv` from Saleae Logic 2 first (File → Export Raw Data).

## Next steps

- Bring up the Pico on the tub (docs/pico2w-pinout.md), including the first **+** press test.
- Identify **slot 5** and **J1-A pins 1–3** using the diagnostic entities.
- Power monitoring (parked): CircuitSetup Split Single Phase Energy Meter with 2 clamps on L1/L2.
