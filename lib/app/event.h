#pragma once

// Application events. Pure data, no platform headers, so the state machine
// that consumes them can be unit-tested on the host.

#include <stdint.h>

enum class AlertType : uint8_t {
    None = 0,
    Emergency = 1,
    Medical = 2,
    Family = 3,
};

enum class EventType : uint8_t {
    ButtonPressed,    // debounced press edge; also the cancel gesture
    ButtonHeld,       // press held past the deliberate-hold threshold
    ButtonReleased,   // debounced release edge; arms the cancel lockout
    Tick,             // periodic time source for timeouts
    AlertSendOk,      // transport accepted the alert for alert_id
    AlertSendFailed,  // transport tried to send alert_id and failed (counts as an attempt)
    AlertSendNotReady,  // nothing sent: host not connected+subscribed (attempt refunded)
    HostAck,          // host acknowledged alert_id
    LinkUp,           // host can receive alerts (BLE: connected and subscribed to Alert)
    LinkDown,         // host link (BLE) disconnected
};

struct Event {
    EventType type;
    uint32_t timestamp_ms;
    AlertType alert;    // ButtonPressed / ButtonHeld / ButtonReleased
    uint16_t alert_id;  // AlertSendOk / AlertSendFailed / AlertSendNotReady / HostAck
};

const char* toString(AlertType type);
const char* toString(EventType type);

// Accepts 1..3 (the physical button numbering); anything else is None.
AlertType alertTypeFromIndex(int index);
