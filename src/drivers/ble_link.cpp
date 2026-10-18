#include "ble_link.h"

#include <Arduino.h>
#include <ArduinoBLE.h>

#include "alert_protocol.h"
#include "diagnostics.h"

namespace {

constexpr const char* kDeviceName = "WEA-Nano33";

// 128-bit vendor UUIDs; only the first group differs between attributes.
constexpr const char* kServiceUuid = "8f1d0001-6c2a-4b7e-9e3d-5a4c3b2a1f00";
constexpr const char* kAlertUuid = "8f1d0002-6c2a-4b7e-9e3d-5a4c3b2a1f00";
constexpr const char* kAckUuid = "8f1d0003-6c2a-4b7e-9e3d-5a4c3b2a1f00";
constexpr const char* kStatusUuid = "8f1d0004-6c2a-4b7e-9e3d-5a4c3b2a1f00";

// Larger than a valid ack so malformed writes arrive intact enough to report
// their real length instead of being clipped to look valid.
constexpr int kAckBufferSize = 20;

BLEService g_service(kServiceUuid);
BLECharacteristic g_alert_char(kAlertUuid, BLERead | BLENotify, protocol::kAlertPacketSize, true);
BLECharacteristic g_ack_char(kAckUuid, BLEWrite | BLEWriteWithoutResponse, kAckBufferSize, false);
BLECharacteristic g_status_char(kStatusUuid, BLERead | BLENotify, protocol::kStatusPacketSize, true);

// ArduinoBLE callbacks are plain function pointers, so they reach the single
// BleLink instance through this pointer (set once in begin()).
BleLink* g_link = nullptr;

void onConnected(BLEDevice central) { g_link->handleConnected(central.address().c_str()); }
void onDisconnected(BLEDevice central) { g_link->handleDisconnected(central.address().c_str()); }
void onAlertSubscribed(BLEDevice, BLECharacteristic) { g_link->handleSubscription("alert", true); }
void onAlertUnsubscribed(BLEDevice, BLECharacteristic) { g_link->handleSubscription("alert", false); }
void onStatusSubscribed(BLEDevice, BLECharacteristic) { g_link->handleSubscription("status", true); }
void onStatusUnsubscribed(BLEDevice, BLECharacteristic) { g_link->handleSubscription("status", false); }
void onAckWritten(BLEDevice, BLECharacteristic characteristic) {
    g_link->handleAckWrite(characteristic.value(), characteristic.valueLength());
}

}  // namespace

BleLink::BleLink(AppEventQueue& events) : events_(events) {}

bool BleLink::begin() {
    g_link = this;
    if (!BLE.begin()) {
        diag::log("BLE_ERR", "op=begin reason=init_failed");
        return false;
    }
    BLE.setLocalName(kDeviceName);
    BLE.setDeviceName(kDeviceName);
    BLE.setAdvertisedService(g_service);

    g_service.addCharacteristic(g_alert_char);
    g_service.addCharacteristic(g_ack_char);
    g_service.addCharacteristic(g_status_char);
    BLE.addService(g_service);

    BLE.setEventHandler(BLEConnected, onConnected);
    BLE.setEventHandler(BLEDisconnected, onDisconnected);
    g_alert_char.setEventHandler(BLESubscribed, onAlertSubscribed);
    g_alert_char.setEventHandler(BLEUnsubscribed, onAlertUnsubscribed);
    g_status_char.setEventHandler(BLESubscribed, onStatusSubscribed);
    g_status_char.setEventHandler(BLEUnsubscribed, onStatusUnsubscribed);
    g_ack_char.setEventHandler(BLEWritten, onAckWritten);

    // ArduinoBLE resumes advertising by itself after a disconnect.
    if (!BLE.advertise()) {
        diag::log("BLE_ERR", "op=advertise reason=failed");
        return false;
    }
    started_ = true;
    diag::log("BLE", "ev=advertising name=%s addr=%s", kDeviceName, BLE.address().c_str());
    return true;
}

void BleLink::poll() {
    if (started_) {
        BLE.poll();
    }
}

const char* BleLink::unavailableReason() const {
    if (!started_) {
        return "ble_not_started";
    }
    if (!connected_) {
        return "not_connected";
    }
    // Without a CCCD subscription a notify is silently dropped by the stack,
    // yet writeValue() still reports success, so check explicitly.
    if (!g_alert_char.subscribed()) {
        return "not_subscribed";
    }
    return nullptr;
}

SendResult BleLink::sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) {
    // Readiness is checked here, at send time, not trusted from an earlier
    // LinkUp: the host may have unsubscribed or dropped since.
    const char* reason = unavailableReason();
    SendResult result = reason == nullptr ? SendResult::Sent : SendResult::NotReady;
    if (result == SendResult::Sent) {
        uint8_t packet[protocol::kAlertPacketSize];
        protocol::encodeAlert(packet, alert_id, type, attempt);
        if (g_alert_char.writeValue(packet, sizeof(packet)) <= 0) {
            reason = "notify_failed";
            result = SendResult::Failed;
        }
    }
    diag::log("BLE_TX", "msg=ALERT id=%u alert=%s attempt=%u ok=%d reason=%s", alert_id,
              toString(type), attempt, result == SendResult::Sent ? 1 : 0, reason ? reason : "-");
    return result;
}

bool BleLink::sendCancel(uint16_t alert_id, AlertType type) {
    const char* reason = unavailableReason();
    if (reason == nullptr) {
        uint8_t packet[protocol::kAlertPacketSize];
        protocol::encodeCancel(packet, alert_id, type);
        if (g_alert_char.writeValue(packet, sizeof(packet)) <= 0) {
            reason = "notify_failed";
        }
    }
    diag::log("BLE_TX", "msg=CANCEL id=%u ok=%d reason=%s", alert_id, reason == nullptr ? 1 : 0,
              reason ? reason : "-");
    return reason == nullptr;
}

void BleLink::publishState(State state, uint16_t alert_id, AlertType type, uint8_t attempts) {
    if (!started_) {
        return;
    }
    uint8_t packet[protocol::kStatusPacketSize];
    protocol::encodeStatus(packet, static_cast<uint8_t>(state), alert_id, type, attempts);
    // Always updates the readable value; notifies only if the host subscribed.
    g_status_char.writeValue(packet, sizeof(packet));
}

void BleLink::handleConnected(const char* address) {
    connected_ = true;
    diag::log("BLE", "ev=connected peer=%s", address);
    // Not LinkUp yet: until the host subscribes to Alert, a send would fail
    // (not_subscribed) and waste an attempt of the retry budget.
}

void BleLink::handleDisconnected(const char* address) {
    connected_ = false;
    diag::log("BLE", "ev=disconnected peer=%s", address);
    post(EventType::LinkDown);
}

void BleLink::handleSubscription(const char* characteristic, bool subscribed) {
    diag::log("BLE", "ev=%s char=%s", subscribed ? "subscribed" : "unsubscribed", characteristic);
    if (subscribed && strcmp(characteristic, "alert") == 0) {
        post(EventType::LinkUp);  // the host can receive alerts now (T14 resends at once)
    }
}

void BleLink::handleAckWrite(const uint8_t* data, int length) {
    uint16_t alert_id = 0;
    const protocol::AckError error =
        protocol::decodeAck(data, length < 0 ? 0 : static_cast<size_t>(length), alert_id);
    if (error != protocol::AckError::None) {
        diag::log("BLE_ERR", "op=rx_ack reason=%s len=%d", protocol::toString(error), length);
        return;
    }
    diag::log("BLE_RX", "msg=ACK id=%u", alert_id);
    post(EventType::HostAck, alert_id);
}

void BleLink::post(EventType type, uint16_t alert_id) {
    if (!events_.post(Event{type, millis(), AlertType::None, alert_id})) {
        diag::log("BLE_ERR", "op=post ev=%s reason=queue_full", toString(type));
    }
}
