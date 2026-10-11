#include "alert_protocol.h"

namespace protocol {
namespace {

void putU16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void encodeAlertLike(uint8_t* out, uint8_t msg, uint16_t alert_id, AlertType type, uint8_t attempt) {
    out[0] = kVersion;
    out[1] = msg;
    putU16(&out[2], alert_id);
    out[4] = static_cast<uint8_t>(type);
    out[5] = attempt;
}

}  // namespace

void encodeAlert(uint8_t out[kAlertPacketSize], uint16_t alert_id, AlertType type, uint8_t attempt) {
    encodeAlertLike(out, kMsgAlert, alert_id, type, attempt);
}

void encodeCancel(uint8_t out[kAlertPacketSize], uint16_t alert_id, AlertType type) {
    encodeAlertLike(out, kMsgCancel, alert_id, type, 0);
}

void encodeStatus(uint8_t out[kStatusPacketSize], uint8_t state, uint16_t alert_id, AlertType type,
                  uint8_t attempts) {
    out[0] = kVersion;
    out[1] = state;
    putU16(&out[2], alert_id);
    out[4] = static_cast<uint8_t>(type);
    out[5] = attempts;
}

AckError decodeAck(const uint8_t* data, size_t length, uint16_t& alert_id) {
    if (data == nullptr || length != kAckPacketSize) {
        return AckError::BadLength;
    }
    if (data[0] != kVersion) {
        return AckError::BadVersion;
    }
    if (data[1] != kMsgAck) {
        return AckError::BadType;
    }
    const uint16_t id = static_cast<uint16_t>(data[2] | (data[3] << 8));
    if (id == 0) {
        return AckError::BadId;  // 0 means "no alert" and can never be acknowledged
    }
    alert_id = id;
    return AckError::None;
}

const char* toString(AckError error) {
    switch (error) {
        case AckError::None: return "none";
        case AckError::BadLength: return "bad_length";
        case AckError::BadVersion: return "bad_version";
        case AckError::BadType: return "bad_type";
        case AckError::BadId: return "bad_id";
    }
    return "unknown";
}

}  // namespace protocol
