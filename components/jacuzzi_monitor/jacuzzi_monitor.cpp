#include "jacuzzi_monitor.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>
#include <esp_system.h>
#include <esp_random.h>
#include "driver/gpio.h"

namespace esphome {
namespace jacuzzi_monitor {

static const char *const TAG = "jacuzzi_monitor";

// ── ESPHome lifecycle ────────────────────────────────────────────────────────

void JacuzziMonitor::setup() {
  // Configure TX pin as GPIO output and drive it low so the U034 DE/RE
  // circuit sees the line as active from the moment we start up, keeping
  // the receiver enabled before any UART frame is transmitted.
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_2, 0);

  ESP_LOGI(TAG, "Jacuzzi J355 RS-485 bus participant starting — passive listen mode");
  bus_state_ = BusState::LISTEN_FOR_CTS;

  // Blind TX bootstrap disabled — passive listen only until frame structure
  // at 38400/7N1 is understood and join sequence is verified.
  // uint32_t r = esp_random();
  // uint8_t payload[3] = {0x02, static_cast<uint8_t>(r & 0xFF), static_cast<uint8_t>((r >> 8) & 0xFF)};
  // ESP_LOGI(TAG, "Blind join request on boot (TX bootstrap, rand=%02X %02X)", payload[1], payload[2]);
  // send_frame_(CH_ASSIGNMENT, MSG_JOIN_REQ, payload, sizeof(payload));
  // blind_tx_ms_ = millis();
}

void JacuzziMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Jacuzzi Monitor: Balboa RS-485 bus participant");
  if (bus_state_ == BusState::JOINED) {
    ESP_LOGCONFIG(TAG, "  Channel: 0x%02X (joined)", my_channel_);
  } else {
    ESP_LOGCONFIG(TAG, "  Channel: not yet joined");
  }
}

// ── Main loop ────────────────────────────────────────────────────────────────
//
// Receives bytes, strips the mark-parity MSB (bit 7 always set by isolated
// transceiver), then uses 0x7E as the Balboa frame delimiter.  Every byte is
// logged at DEBUG so we can see exactly what arrives on the wire.
//
void JacuzziMonitor::loop() {
  uint32_t now = millis();

  // ── Timeout: waiting for assignment response ──────────────────────────────
  if (bus_state_ == BusState::WAIT_FOR_ASSIGNMENT) {
    if ((now - join_attempt_ms_) > 3000) {
      ESP_LOGW(TAG, "Channel assignment timed out — resetting to listen");
      bus_state_ = BusState::LISTEN_FOR_CTS;
    }
  }

  // ── Timeout: no CTS received for >5 s while joined ───────────────────────
  if (bus_state_ == BusState::JOINED && last_cts_ms_ > 0) {
    if ((now - last_cts_ms_) > 5000) {
      ESP_LOGW(TAG, "CTS timeout (>5 s without CTS) — resetting and rejoining");
      bus_state_   = BusState::LISTEN_FOR_CTS;
      my_channel_  = 0;
      last_cts_ms_ = 0;
    }
  }

  // ── Receive bytes ─────────────────────────────────────────────────────────
  while (available()) {
    uint8_t b;
    if (!read_byte(&b))
      break;

    b &= 0x7F;  // strip mark-parity MSB (isolated transceiver always sets bit 7)

    ESP_LOGD(TAG, "RX 0x%02X", b);

    if (b == 0x7E) {
      // Frame delimiter: complete frame is whatever is in the buffer
      if (buf_len_ > 0) {
        flush_packet_();
        process_frame_(buf_, buf_len_);
        buf_len_ = 0;
      }
      last_byte_ms_ = 0;
      continue;
    }

    // Gap since last non-delimiter byte → orphaned bytes before a delimiter
    if (buf_len_ > 0 && (now - last_byte_ms_) > PACKET_GAP_MS) {
      ESP_LOGW(TAG, "GAP-FLUSH (no 7E) %u bytes | %s",
               (unsigned)buf_len_, hex_str_(buf_, buf_len_).c_str());
      buf_len_ = 0;
    }

    if (buf_len_ < MAX_FRAME) {
      buf_[buf_len_++] = b;
    } else {
      ESP_LOGW(TAG, "Packet exceeds %u-byte buffer, discarding", (unsigned)MAX_FRAME);
      buf_len_ = 0;
    }
    last_byte_ms_ = now;
  }

  // Flush bytes that never got a closing 7E
  if (buf_len_ > 0 && last_byte_ms_ > 0 && (now - last_byte_ms_) > PACKET_GAP_MS) {
    ESP_LOGW(TAG, "TIMEOUT-FLUSH (no 7E) %u bytes | %s",
             (unsigned)buf_len_, hex_str_(buf_, buf_len_).c_str());
    buf_len_ = 0;
  }
}

// ── Packet flusher ───────────────────────────────────────────────────────────

void JacuzziMonitor::flush_packet_() {
  packet_count_++;
  ESP_LOGI(TAG, "FRAME[%u] %u bytes | %s",
           (unsigned)packet_count_,
           (unsigned)buf_len_, hex_str_(buf_, buf_len_).c_str());
}

// ── Frame processor ──────────────────────────────────────────────────────────

void JacuzziMonitor::process_frame_(const uint8_t *data, size_t len) {
  if (len < 5)
    return;

  uint8_t channel  = data[1];
  uint8_t ind      = data[2];
  uint8_t msg_type = data[3];

  uint8_t expected_ind = (channel == CH_BROADCAST) ? 0xAF : 0xBF;
  if (ind != expected_ind) {
    ESP_LOGW(TAG, "Unexpected indicator 0x%02X for ch=0x%02X (expected 0x%02X)",
             ind, channel, expected_ind);
  }

  // CRC check: covers data[0 .. len-2]; CRC is data[len-1].
  uint8_t computed = compute_crc_(data, len - 1);
  uint8_t received = data[len - 1];
  if (computed != received) {
    ESP_LOGW(TAG, "CRC FAIL ch=0x%02X type=0x%02X got=0x%02X calc=0x%02X | %s",
             channel, msg_type, received, computed,
             hex_str_(data, len).c_str());
    return;
  }

  // Log every valid received frame at INFO
  ESP_LOGI(TAG, "<<< ch=0x%02X type=0x%02X len=%u | %s",
           channel, msg_type, (unsigned) len, hex_str_(data, len).c_str());

  // ── Dispatch ─────────────────────────────────────────────────────────────
  if (channel == CH_ASSIGNMENT && msg_type == MSG_NEW_CLIENT) {
    handle_new_client_cts_();
  } else if (channel == CH_ASSIGNMENT && msg_type == MSG_JOIN_RESP) {
    handle_channel_assignment_(data, len);
  } else if (channel == my_channel_ && msg_type == MSG_CTS
             && bus_state_ == BusState::JOINED) {
    handle_cts_();
  } else if (channel == CH_BROADCAST && msg_type == MSG_STATUS) {
    decode_status_update_(data, len);
  }
}

// ── Bus-join handlers ────────────────────────────────────────────────────────

void JacuzziMonitor::handle_new_client_cts_() {
  if (bus_state_ != BusState::LISTEN_FOR_CTS) {
    return;  // Already joining or joined — ignore
  }

  // Two random bytes for collision detection on the assignment channel
  uint32_t r    = esp_random();
  join_rand_[0] = static_cast<uint8_t>(r & 0xFF);
  join_rand_[1] = static_cast<uint8_t>((r >> 8) & 0xFF);

  ESP_LOGI(TAG, "New-client CTS — sending join request (rand=%02X %02X)",
           join_rand_[0], join_rand_[1]);

  // Payload: [0x02, r1, r2]  (0x02 = client type identifier per Balboa spec)
  uint8_t payload[3] = {0x02, join_rand_[0], join_rand_[1]};
  send_frame_(CH_ASSIGNMENT, MSG_JOIN_REQ, payload, sizeof(payload));

  bus_state_       = BusState::WAIT_FOR_ASSIGNMENT;
  join_attempt_ms_ = millis();
}

void JacuzziMonitor::handle_channel_assignment_(const uint8_t *data, size_t len) {
  if (bus_state_ != BusState::WAIT_FOR_ASSIGNMENT) {
    return;
  }

  // Response frame: [len][FE][BF][02][assigned_ch][r1][r2][CRC]
  //   data[4] = assigned channel
  //   data[5] = echo of join_rand_[0]
  //   data[6] = echo of join_rand_[1]
  if (len < 6) {
    ESP_LOGW(TAG, "Assignment response too short: %u bytes", (unsigned) len);
    bus_state_ = BusState::LISTEN_FOR_CTS;
    return;
  }

  uint8_t assigned = data[4];

  // Verify echoed random bytes to confirm this assignment is for us
  if (len >= 8) {
    if (data[5] != join_rand_[0] || data[6] != join_rand_[1]) {
      ESP_LOGW(TAG,
               "Assignment random mismatch (ours=%02X %02X, got=%02X %02X) — not for us",
               join_rand_[0], join_rand_[1], data[5], data[6]);
      bus_state_ = BusState::LISTEN_FOR_CTS;
      return;
    }
  }

  my_channel_ = assigned;
  ESP_LOGI(TAG, "Assigned channel 0x%02X — sending ACK", my_channel_);
  send_frame_(my_channel_, MSG_JOIN_ACK, nullptr, 0);

  bus_state_   = BusState::JOINED;
  last_cts_ms_ = millis();
  ESP_LOGI(TAG, "Bus join complete — active on channel 0x%02X", my_channel_);
}

void JacuzziMonitor::handle_cts_() {
  last_cts_ms_ = millis();
  send_frame_(my_channel_, MSG_NTS, nullptr, 0);
}

// ── Status Update decoder (CH_BROADCAST / MSG_STATUS) ───────────────────────
//
// Payload byte offsets are 0-indexed from the first byte AFTER the 4-byte
// header (len / channel / indicator / type), i.e. p[N] = data[N + 4].
//
//   p[2]     — current temperature (raw; 0xFF = sensor not ready)
//   p[5]     — heating mode (bits 1-0: 0=Ready 1=Rest 3=Ready-in-Rest)
//   p[9]     — flags: bit 0 = temp scale (0=°F, 1=°C/0.5° steps)
//   p[10]    — heating state (bits 5-4: 0=Off 1=Heating 2=HeatWaiting)
//   p[11]    — pump states: P1=bits 1-0  P2=bits 3-2  (0=Off 1=Low 2=High)
//   p[14]    — lights: L1=bits 1-0  (0=Off, non-zero=On)
//   p[20]    — set temperature (raw, same scale as current)
//
void JacuzziMonitor::decode_status_update_(const uint8_t *data, size_t len) {
  static const size_t HEADER      = 4;   // len + ch + ind + type
  static const size_t PAYLOAD_MIN = 21;  // need p[0] through p[20]
  static const size_t MIN_LEN     = HEADER + PAYLOAD_MIN + 1;  // +1 for CRC

  if (len < MIN_LEN) {
    ESP_LOGW(TAG, "Status update too short: %u bytes (need %u)",
             (unsigned) len, (unsigned) MIN_LEN);
    return;
  }

  const uint8_t *p = data + HEADER;

  // Temperature scale
  bool        celsius = (p[9] & 0x01) != 0;
  const char *unit    = celsius ? "C" : "F";

  // Current temperature
  uint8_t cur_raw = p[2];
  char    cur_str[8];
  if (cur_raw == 0xFF) {
    snprintf(cur_str, sizeof(cur_str), "---");
  } else if (celsius) {
    snprintf(cur_str, sizeof(cur_str), "%.1f", cur_raw / 2.0f);
  } else {
    snprintf(cur_str, sizeof(cur_str), "%u", (unsigned) cur_raw);
  }

  // Set temperature
  uint8_t set_raw = p[20];
  char    set_str[8];
  if (celsius) {
    snprintf(set_str, sizeof(set_str), "%.1f", set_raw / 2.0f);
  } else {
    snprintf(set_str, sizeof(set_str), "%u", (unsigned) set_raw);
  }

  // Heating mode
  static const char *const HEAT_MODES[4]  = {"Ready", "Rest", "?2", "Ready-in-Rest"};
  uint8_t heat_mode  = p[5] & 0x03;

  // Heating state
  static const char *const HEAT_STATES[4] = {"Off", "Heating", "HeatWaiting", "?3"};
  uint8_t heat_state = (p[10] >> 4) & 0x03;

  // Pump states (2-bit pairs in p[11])
  static const char *const PUMP_STATES[4] = {"Off", "Low", "High", "?P"};
  uint8_t p1 =  p[11]       & 0x03;
  uint8_t p2 = (p[11] >> 2) & 0x03;

  // Lights (L1 in bits 1-0 of p[14])
  bool lights = (p[14] & 0x03) != 0;

  ESP_LOGI(TAG,
           "STATUS cur=%s%s set=%s%s | mode=%s heat=%s | P1=%s P2=%s | lights=%s",
           cur_str, unit,
           set_str, unit,
           HEAT_MODES[heat_mode],
           HEAT_STATES[heat_state],
           PUMP_STATES[p1],
           PUMP_STATES[p2],
           lights ? "on" : "off");
}

// ── TX frame builder ─────────────────────────────────────────────────────────
//
// Writes:  0x7E [len][ch][ind][type][payload...][CRC] 0x7E  (delimiter TBD)
//
// len = 1(len) + 1(ch) + 1(ind) + 1(type) + payload_len + 1(CRC)
//
void JacuzziMonitor::send_frame_(uint8_t channel, uint8_t type,
                                  const uint8_t *payload, size_t payload_len) {
  auto len = static_cast<uint8_t>(5 + payload_len);
  uint8_t ind = (channel == CH_BROADCAST) ? 0xAF : 0xBF;

  uint8_t frame[MAX_TX_FRAME];
  frame[0] = len;
  frame[1] = channel;
  frame[2] = ind;
  frame[3] = type;
  if (payload_len > 0 && payload != nullptr) {
    memcpy(frame + 4, payload, payload_len);
  }
  // CRC covers frame[0 .. len-2].
  frame[4 + payload_len] = compute_crc_(frame, 4 + payload_len);

  // Log outgoing frame at INFO before transmitting
  ESP_LOGI(TAG, ">>> ch=0x%02X type=0x%02X len=%u | %s",
           channel, type, (unsigned) len, hex_str_(frame, len).c_str());

  write_byte(0x7E);  // TX delimiter — verify once RX framing is understood
  write_array(frame, len);
  write_byte(0x7E);  // TX delimiter — verify once RX framing is understood
  flush();  // Block until TX FIFO is empty before reclaiming the pin
  // Hold TX low so U034 DE/RE stays low (receiver enabled) until next frame.
  // The UART peripheral reclaims GPIO2 automatically when it starts transmitting.
  gpio_set_level(GPIO_NUM_2, 0);
}

// ── CRC-8 ────────────────────────────────────────────────────────────────────
//
// Polynomial 0x07, initial value 0x02, final XOR 0x02.
// Input covers all inner-frame bytes from len through last payload byte
// (i.e. everything except the CRC byte itself).
//
uint8_t JacuzziMonitor::compute_crc_(const uint8_t *data, size_t len) {
  uint8_t crc = 0x02;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc ^ 0x02;
}

// ── Hex string formatter ─────────────────────────────────────────────────────

std::string JacuzziMonitor::hex_str_(const uint8_t *data, size_t len) {
  const size_t cap = (len > 32) ? 32 : len;
  std::string  out;
  out.reserve(cap * 3);
  char tmp[4];
  for (size_t i = 0; i < cap; i++) {
    if (i) out += ' ';
    snprintf(tmp, sizeof(tmp), "%02X", data[i]);
    out += tmp;
  }
  if (len > cap)
    out += " ...";
  return out;
}

}  // namespace jacuzzi_monitor
}  // namespace esphome
