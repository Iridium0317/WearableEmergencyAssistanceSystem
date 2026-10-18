#pragma once

// Boundary between the application and whatever carries alerts to the host
// (BleLink on hardware, SimulatedLink for console-driven tests).
//
// All methods are called from the app thread only. That is a hard requirement
// for BleLink: ArduinoBLE is not thread-safe, so the thread that polls it must
// be the only one that touches it.

#include <stdint.h>

#include "event.h"
#include "state_machine.h"

// Outcome of one send. Only Sent and Failed count against the attempt budget:
// NotReady means nothing left the device because the host cannot receive yet.
enum class SendResult : uint8_t {
    Sent,      // handed to the link (delivery is still unconfirmed until HostAck)
    NotReady,  // host not connected and subscribed; nothing transmitted
    Failed,    // host looked ready but the link refused the packet
};

class AlertTransport {
public:
    virtual ~AlertTransport() = default;

    virtual bool begin() { return true; }

    // Services the link; incoming traffic becomes events on the app queue.
    virtual void poll() {}

    // Checks host readiness first. Delivery is confirmed only by a later
    // HostAck event, never by this return value.
    virtual SendResult sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) = 0;
    virtual bool sendCancel(uint16_t alert_id, AlertType type) = 0;

    // Mirrors the state machine to the host after every transition.
    virtual void publishState(State state, uint16_t alert_id, AlertType type, uint8_t attempts) {
        (void)state;
        (void)alert_id;
        (void)type;
        (void)attempts;
    }

    virtual const char* name() const = 0;
};
