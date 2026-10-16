#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace jacuzzi_monitor {

class JacuzziMonitor : public Component, public uart::UARTDevice {
 public:
  // ── ESPHome lifecycle ────────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 private:
  // ── Constants ────────────────────────────────────────────────────────────
  static constexpr size_t   MAX_FRAME     = 256;  // Packet accumulation buffer
  static constexpr size_t   MAX_TX_FRAME  = 32;   // Max TX frame bytes
  static constexpr uint32_t PACKET_GAP_MS = 10;   // Silence threshold for packet boundary

  // ── Channel / message-type constants ────────────────────────────────────
  static constexpr uint8_t CH_BROADCAST    = 0xFF;  // Broadcast channel
  static constexpr uint8_t CH_ASSIGNMENT   = 0xFE;  // Join handshake channel

  static constexpr uint8_t MSG_NEW_CLIENT  = 0x00;  // Main → all: new-client CTS
  static constexpr uint8_t MSG_JOIN_REQ    = 0x01;  // Us → main: channel request
  static constexpr uint8_t MSG_JOIN_RESP   = 0x02;  // Main → us: channel assigned
  static constexpr uint8_t MSG_JOIN_ACK    = 0x03;  // Us → main: ACK assignment
  static constexpr uint8_t MSG_CTS         = 0x06;  // Main → us: clear to send
  static constexpr uint8_t MSG_NTS         = 0x07;  // Us → main: nothing to send
  static constexpr uint8_t MSG_STATUS      = 0x13;  // Main → all: spa status

  // ── Bus join state machine ───────────────────────────────────────────────
  enum class BusState : uint8_t {
    LISTEN_FOR_CTS,       // Waiting for new-client CTS on CH_ASSIGNMENT
    WAIT_FOR_ASSIGNMENT,  // Join request sent; awaiting channel response
    JOINED,               // Active on bus; responding to CTS with NTS
  };

  BusState bus_state_{BusState::LISTEN_FOR_CTS};

  uint8_t  my_channel_{0};       // Assigned channel (valid only in JOINED state)
  uint8_t  join_rand_[2]{};      // Random bytes included in our join request
  uint32_t last_cts_ms_{0};      // millis() of last CTS received on our channel
  uint32_t join_attempt_ms_{0};  // millis() when join request was sent
  uint32_t blind_tx_ms_{0};      // millis() of last blind TX bootstrap transmit

  // ── Packet accumulator ──────────────────────────────────────────────────
  uint8_t  buf_[MAX_FRAME]{};      // Non-zero bytes accumulated since last gap
  size_t   buf_len_{0};            // Bytes in buf_ so far
  uint32_t last_byte_ms_{0};       // millis() of last received byte (gap timer)
  uint32_t packet_count_{0};       // Running packet index for log messages

  // ── Helpers ──────────────────────────────────────────────────────────────

  // CRC-8: polynomial 0x07, initial 0x02, final XOR 0x02.
  // Covers all inner-frame bytes from len through last payload byte (excludes CRC).
  uint8_t compute_crc_(const uint8_t *data, size_t len);

  // Build and transmit a framed message; logs the outgoing frame at INFO level.
  void send_frame_(uint8_t channel, uint8_t type,
                   const uint8_t *payload, size_t payload_len);

  // Log accumulated packet bytes at INFO and reset the buffer.
  void flush_packet_();

  // Validate CRC and dispatch to handlers below.
  void process_frame_(const uint8_t *data, size_t len);

  // Bus-join handlers
  void handle_new_client_cts_();
  void handle_channel_assignment_(const uint8_t *data, size_t len);
  void handle_cts_();

  // Spa state decoder (CH_BROADCAST / MSG_STATUS)
  void decode_status_update_(const uint8_t *data, size_t len);

  // Utility: space-separated hex string (capped at 32 bytes)
  std::string hex_str_(const uint8_t *data, size_t len);
};

}  // namespace jacuzzi_monitor
}  // namespace esphome
