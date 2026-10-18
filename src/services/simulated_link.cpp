#include "simulated_link.h"

#include "diagnostics.h"

SendResult SimulatedLink::sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) {
    const bool up = isUp();
    diag::log("LINK", "tx=ALERT id=%u alert=%s attempt=%u up=%d",
              alert_id, toString(type), attempt, up ? 1 : 0);
    // A simulated link that is down behaves like a host that is not connected.
    return up ? SendResult::Sent : SendResult::NotReady;
}

bool SimulatedLink::sendCancel(uint16_t alert_id, AlertType type) {
    (void)type;
    const bool up = isUp();
    diag::log("LINK", "tx=CANCEL id=%u up=%d", alert_id, up ? 1 : 0);
    return up;
}
