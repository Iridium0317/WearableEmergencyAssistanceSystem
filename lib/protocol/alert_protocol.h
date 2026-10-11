#pragma once

// Wire format for the BLE emergency service. Transport-agnostic and pure so
// encoding/validation can be unit-tested on the host.
//
// All multi-byte fields are little-endian. Every packet starts with a version
// byte and a message type so the format can evolve without ambiguity. There
// is no application checksum: BLE already protects each PDU with a 24-bit
// link-layer CRC. What BLE does NOT give us is delivery confirmation for
// notifications, which is why the host must send an explicit Ack.
//
// Device -> host, Alert characteristic (notify), 6 bytes:
//   [0] version (1)  [1] type: 0x01 ALERT, 0x02 CANCEL
//   [2..3] alert_id  [4] alert type (1..3)  [5] attempt (1..n, 0 for CANCEL)
//
// Device -> host, Status characteristic (read/notify), 6 bytes:
//   [0] version (1)  [1] state (State enum value)
//   [2..3] alert_id  [4] alert type  [5] attempts so far
//
// Host -> device, Ack characteristic (write), exactly 4 bytes:
//   [0] version (1)  [1] type: 0x81 ACK  [2..3] alert_id (non-zero)

#include <stddef.h>
#include <stdint.h>

#include "event.h"

namespace protocol {

constexpr uint8_t kVersion = 1;

constexpr uint8_t kMsgAlert = 0x01;
constexpr uint8_t kMsgCancel = 0x02;
constexpr uint8_t kMsgAck = 0x81;

constexpr size_t kAlertPacketSize = 6;
constexpr size_t kStatusPacketSize = 6;
constexpr size_t kAckPacketSize = 4;

void encodeAlert(uint8_t out[kAlertPacketSize], uint16_t alert_id, AlertType type, uint8_t attempt);
void encodeCancel(uint8_t out[kAlertPacketSize], uint16_t alert_id, AlertType type);
void encodeStatus(uint8_t out[kStatusPacketSize], uint8_t state, uint16_t alert_id, AlertType type,
                  uint8_t attempts);

enum class AckError : uint8_t {
    None,
    BadLength,
    BadVersion,
    BadType,
    BadId,
};

// Validates every field; on success writes the acknowledged id to alert_id.
AckError decodeAck(const uint8_t* data, size_t length, uint16_t& alert_id);

const char* toString(AckError error);

}  // namespace protocol
