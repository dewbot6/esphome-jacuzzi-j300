> **Historical (April 2026).** Early notes from the M5Stack AtomS3 attempt. That hardware was abandoned.

ESPHome passive RS-485 monitor for a Jacuzzi J355 hot tub, targeting an M5Stack AtomS3 Lite (ESP32-S3).

**Why:** Decode the Balboa-protocol RS-485 bus on the J355 accessory port to expose tub state to Home Assistant.

**How to apply:** Keep board, GPIO assignments, and component code consistent with the config described here when suggesting changes.

## Board / framework

- Board: `m5stack-atoms3-lite`
- Variant: `esp32s3`
- Framework: `esp-idf`, version: recommended
- Connectivity: plain WiFi (no OpenThread / Thread)

## Files

- `jacuzzi_monitor.yaml` — main ESPHome config
- `secrets.yaml` — wifi_ssid, wifi_password, ap_password, ota_password
- `components/jacuzzi_monitor/` (or `balboa_monitor/`) — custom ESPHome component with decoding logic

## GPIO / wiring

M5Stack AtomS3 Lite Grove port:

| Grove pin | Wire   | GPIO  | Role        |
|-----------|--------|-------|-------------|
| 1         | Yellow | GPIO2 | UART RX     |
| 2         | White  | GPIO1 | UART TX (unused — listen-only) |
| 3         | Red    | —     | 3V3         |
| 4         | Black  | —     | GND         |

Note: AtomS3 Lite is **opposite** to NanoC6 — NanoC6 had GPIO1=yellow, GPIO2=white.

## UART settings

- baud_rate: 115200 (62500 is commented as an alternative to try if frames don't appear)
- data_bits: 8, parity: NONE, stop_bits: 1
- rx_buffer_size: 512
- debug: direction RX, dummy_receiver false

## RS-485 hardware path

J355 RJ45 accessory port (pins 4/5 = A/B) → M5Stack RS485-to-TTL Unit (U094, auto-direction) → Grove cable → AtomS3 Lite
