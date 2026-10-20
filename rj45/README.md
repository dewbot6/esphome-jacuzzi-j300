# Backup method: Waveshare ESP32 on the RJ45 port (read-only)

> The main method is the Pico 2 W on the topside panel connector J1-A. See the
> [top-level README](../README.md). This folder keeps the earlier, **working** read-only
> setup, which is deployed at the tub. It needs no access to the panel cable.

The RJ45 port carries the panel bus **clock** (pin 4) but not the data line. Every data bit
leaks through the clock timing anyway: the board's processor takes 2 µs less to send a 1
(15 µs between pulses) than a 0 (17 µs). This firmware times the clock with the ESP32's
RMT peripheral and rebuilds the frame. It never transmits.

Entities: Water Temperature, Set Temperature, Display, Heater, Jets 1, Jets 2,
Status Slot 5, Raw Frame. No control.

## Hardware and wiring

Waveshare ESP32-S3-RS485-CAN. Its isolated RS-485 receiver is used as a level shifter.

| Waveshare RS-485 terminal | RJ45 pin | Signal |
|---|---|---|
| **A+** | **3** | ground |
| **B−** | **4** | panel bus clock |
| GND | 3 | ground |

- The A/B order matters. A+ on pin 4 doesn't work (the receiver's fail-safe zone).
- Leave the **120R termination jumper off**. EN (GPIO21) is held low (receive only).
- Don't connect anything else. Transmitting on these lines once caused a **C96 error** and a
  stuck recirc pump.

## Flashing

`rj45/secrets.yaml` is a symlink to the top-level `secrets.yaml`, which needs
`api_encryption_key`. From this folder:

```
esphome run jacuzzi_monitor.yaml                      # over Wi-Fi (jacuzzi-monitor.local)
esphome run jacuzzi_monitor.yaml --device /dev/cu.usbmodem…   # first flash over USB
```

If the symlink is missing (e.g. a fresh clone on Windows), copy `../secrets.example.yaml`
to `rj45/secrets.yaml` and fill it in.

## Contents

| Path | Contents |
|---|---|
| `jacuzzi_monitor.yaml`, `components/jacuzzi_monitor/` | Waveshare firmware |
| `docs/rj45-timing-decode.md` | RJ45 pins, timing decode, why some pulses vanish through the receiver |
| `docs/scope/` | May 2026 oscilloscope shots of the RJ45 lines |
| `captures/*.sal` | Logic 8 captures on the RJ45 port (2026-10-16) |
| `tools/slots.py` | Decode slot bits from clock timing in a capture |
| `tools/sim_receiver.py` | Replay a capture as the Waveshare sees it, through the firmware logic |
| `tools/frames.py` | Raw per-frame timing dump |
