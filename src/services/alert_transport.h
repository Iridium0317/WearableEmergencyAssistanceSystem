#pragma once

// Boundary between the application and whatever carries alerts to the host.
// SimulatedLink implements it today; the BLE service will implement it next,
// without the controller or state machine changing.

#include <stdint.h>

#include "event.h"

class AlertTransport {
public:
    virtual ~AlertTransport() = default;

    // Returns true if the alert was handed to the link. Delivery is confirmed
    // only by a later HostAck event, never by this return value.
    virtual bool sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) = 0;
    virtual bool sendCancel(uint16_t alert_id) = 0;
    virtual const char* name() const = 0;
};
