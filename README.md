# Jacuzzi J355 → Home Assistant

A **listen-only** ESPHome monitor for a Jacuzzi J355 hot tub (J-300 series main board,
PCB `6600-087 REV D`, firmware `REV 3.61A`). It plugs into the RJ45 port on the main
board and reports the tub's state to Home Assistant. It never transmits anything to the tub.

## What shows up in Home Assistant

| Entity | What it is |
|---|---|
| **Water Temperature** | °F, as shown on the topside display |
| **Set Temperature** | °F. The tub only sends it while the set point is on the display, so it updates a few seconds after someone presses + or −. Unknown after a reboot until the next press. |
| **Heater** | On when the heater is running (the red dot on the panel) |
| **Jets 1**, **Jets 2** | On/off |
| Display | Diagnostic: the number currently on the panel |
| Status Slot 5 | Diagnostic: an unidentified status bit (maybe the circulation pump) |
| Raw Frame | Diagnostic: the 23 decoded bits |

**Not available:** the light (on/off and color). The lights have their own cable from the topside panel and never touch this bus. **Control**
(changing the set point, jets or light from HA) isn't implemented. See [Next steps](#next-steps).

## Hardware

- **Waveshare ESP32-S3-RS485-CAN**. Its isolated RS-485 receiver is used as a safe
  level shifter.
- An RJ45 screw-terminal breakout on the main board's RJ45 port.
- USB power for the Waveshare.

### Wiring

| Waveshare RS-485 terminal | RJ45 pin | Signal |
|---|---|---|
| **A+** | **3** | ground |
| **B−** | **4** | panel bus clock |
| GND | 3 | ground |

- The A/B order matters. A+ on pin 4 doesn't work.
- Leave the Waveshare's **120R termination jumper off**.
- Don't connect anything else. RJ45 pin 5 is the bus strobe and must not be driven:
  transmitting on these lines once caused a **C96 error** and a stuck recirc pump.

## Flashing

1. Install ESPHome (`pip install esphome`) or use the ESPHome add-on / Builder.
2. `cp secrets.example.yaml secrets.yaml` and fill in Wi-Fi, OTA password and an API
   encryption key (generate one with `openssl rand -base64 32`).
3. First flash over USB: `esphome run jacuzzi_monitor.yaml --device /dev/cu.usbmodem…`
4. Later updates can go over Wi-Fi: `esphome run jacuzzi_monitor.yaml`

## Adding it to Home Assistant

**Settings → Devices & services.** "Jacuzzi J355 Monitor" appears under *Discovered*.
Click **Add** and paste the `api_encryption_key` from `secrets.yaml`. If it isn't
discovered, use **Add Integration → ESPHome** with host `jacuzzi-monitor.local` and port 6053.

## How it works

The main board drives the topside panel with a bit-banged clock/strobe/data bus that
repeats every 100 ms. The data line isn't on the RJ45, but the board's processor takes
2 µs less to send a 1 than a 0. So each bit shows up as the gap between clock pulses:
**15 µs = 1, 17 µs = 0**. The ESP32 times the clock with its RMT peripheral and
rebuilds the frame: heater, jets, and three BCD digits of the display.

Full details: [docs/protocol.md](docs/protocol.md). That includes the slot map and the
odd ways the clock looks through the RS-485 receiver.

## Repository layout

| Path | Contents |
|---|---|
| `jacuzzi_monitor.yaml` | ESPHome device config |
| `components/jacuzzi_monitor/` | The decoder (C++ external component) |
| `docs/protocol.md` | Decoded protocol, slot map, wiring notes |
| `docs/*.webp`, `docs/scope/` | Board photos and early oscilloscope shots |
| `captures/*.sal` | Saleae Logic 8 captures from the RJ45 port (open in Saleae Logic 2) |
| `tools/` | Capture and analysis scripts (see below) |
| `docs/history/` | Earlier notes, kept for reference. Some of their conclusions turned out wrong. |

### Tools

Analysis scripts need Python 3 and `numpy` (`pip install -r tools/requirements.txt`).
To use them on a `.sal` capture, export it from Saleae Logic 2 to `digital.csv` first
(File → Export Raw Data).

- `tools/saleae.py`: drives a Logic 8 through Saleae Logic 2's built-in MCP server
  (127.0.0.1:10530) to record and export captures.
- `tools/slots.py <digital.csv>`: decodes each frame's bits from a Logic 8 capture.
- `tools/sim_receiver.py <digital.csv>`: replays a capture the way the Waveshare sees
  it, through the same decoder and set-point logic as the firmware.
- `tools/frames.py`: raw per-frame timing dump.

## Next steps

- **Identify slot 5.** It flipped from 1 to 0 on 2026-10-16 and stayed there. Watching
  the HA entity alongside the circulation pump should tell.
- **Sniff the topside panel connector (J1-A)** with the Logic 8, listen-only, to see
  the real data line and the button-reply line during the 7-pulse button scan.
- **Control**, if wanted. The safest route is optocouplers across the topside panel's
  buttons, driven by the ESP. Since the display is decoded, set-point changes can be
  confirmed. Another option is a cautious test of RJ45 pins 1/6, which look like unused
  inputs, through a 10 kΩ resistor.
