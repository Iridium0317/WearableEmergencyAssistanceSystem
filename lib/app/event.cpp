#include "event.h"

const char* toString(AlertType type) {
    switch (type) {
        case AlertType::None: return "NONE";
        case AlertType::Emergency: return "EMERGENCY";
        case AlertType::Medical: return "MEDICAL";
        case AlertType::Family: return "FAMILY";
    }
    return "UNKNOWN";
}

const char* toString(EventType type) {
    switch (type) {
        case EventType::ButtonPressed: return "BUTTON_PRESSED";
        case EventType::ButtonHeld: return "BUTTON_HELD";
        case EventType::ButtonReleased: return "BUTTON_RELEASED";
        case EventType::Tick: return "TICK";
        case EventType::AlertSendOk: return "ALERT_SEND_OK";
        case EventType::AlertSendFailed: return "ALERT_SEND_FAILED";
        case EventType::AlertSendNotReady: return "ALERT_SEND_NOT_READY";
        case EventType::HostAck: return "HOST_ACK";
        case EventType::LinkUp: return "LINK_UP";
        case EventType::LinkDown: return "LINK_DOWN";
    }
    return "UNKNOWN";
}

AlertType alertTypeFromIndex(int index) {
    switch (index) {
        case 1: return AlertType::Emergency;
        case 2: return AlertType::Medical;
        case 3: return AlertType::Family;
        default: return AlertType::None;
    }
}
