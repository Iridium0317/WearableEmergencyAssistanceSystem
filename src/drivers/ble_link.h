#pragma once

// BLE GATT transport for alerts (see lib/protocol/alert_protocol.h for the
// packet formats and README for the UUIDs).
//
// Threading: ArduinoBLE is not thread-safe and only delivers callbacks from
// inside BLE.poll(). Every method here, including begin(), must run on the app
// thread; the callbacks therefore also run on the app thread and simply post
// events to the queue. ArduinoBLE types stay out of this header on purpose.

#include <stdint.h>

#include "alert_transport.h"
#include "event_queue.h"

class BleLink : public AlertTransport {
public:
    explicit BleLink(AppEventQueue& events);

    bool begin() override;
    void poll() override;
    SendResult sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) override;
    bool sendCancel(uint16_t alert_id, AlertType type) override;
    void publishState(State state, uint16_t alert_id, AlertType type, uint8_t attempts) override;
    const char* name() const override { return "BLE"; }

    // ArduinoBLE callback targets (invoked from poll(), app thread).
    void handleConnected(const char* address);
    void handleDisconnected(const char* address);
    void handleSubscription(const char* characteristic, bool subscribed);
    void handleAckWrite(const uint8_t* data, int length);

private:
    // Why a send can't go out right now, or nullptr if it can.
    const char* unavailableReason() const;
    void post(EventType type, uint16_t alert_id = 0);

    AppEventQueue& events_;
    bool started_ = false;
    // Tracked here rather than via BLE.connected(): ArduinoBLE still reports
    // the peer as connected while its disconnect callback is running.
    bool connected_ = false;
};
