#include "simulated_link.h"

#include "diagnostics.h"

bool SimulatedLink::sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) {
    const bool up = isUp();
    diag::log("LINK", "tx=ALERT id=%u alert=%s attempt=%u up=%d",
              alert_id, toString(type), attempt, up ? 1 : 0);
    return up;
}

bool SimulatedLink::sendCancel(uint16_t alert_id) {
    const bool up = isUp();
    diag::log("LINK", "tx=CANCEL id=%u up=%d", alert_id, up ? 1 : 0);
    return up;
}
