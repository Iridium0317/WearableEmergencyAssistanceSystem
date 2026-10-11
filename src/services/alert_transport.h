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

class AlertTransport {
public:
    virtual ~AlertTransport() = default;

    virtual bool begin() { return true; }

    // Services the link; incoming traffic becomes events on the app queue.
    virtual void poll() {}

    // Returns true if the alert was handed to the link. Delivery is confirmed
    // only by a later HostAck event, never by this return value.
    virtual bool sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) = 0;
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
