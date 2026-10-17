#pragma once

// Line-based serial command console used by the bench tool to inject events.
//
//   ping              -> CMD_OK ping   (liveness check)
//   btn               -> BTN_DIAG raw pin levels + ISR edge count (wiring check)
//   reboot            -> CMD_OK, then system reset (bench starts from IDLE)
//   hold <1-3>        -> ButtonPressed + ButtonHeld for that alert type
//   press             -> ButtonPressed (the cancel gesture)
//   ack <id>          -> HostAck for alert <id>
//   link up|down      -> simulated host link state + LinkUp/LinkDown event
//                        (simulated-link builds only; CMD_ERR otherwise)
//
// Every line gets exactly one CMD_OK or CMD_ERR reply; malformed input is
// always reported, never dropped silently. Runs in the Arduino loop thread.

#include <stddef.h>

#include "buttons.h"
#include "event_queue.h"
#include "simulated_link.h"

class CommandConsole {
public:
    // link may be null when the real BLE transport is in use.
    CommandConsole(AppEventQueue& events, SimulatedLink* link, ButtonInput& buttons);

    // Non-blocking: consumes whatever serial bytes are available.
    void poll();

private:
    static constexpr size_t kLineSize = 48;

    void execute(char* line);
    bool post(EventType type, AlertType alert = AlertType::None, uint16_t id = 0);

    AppEventQueue& events_;
    SimulatedLink* link_;
    ButtonInput& buttons_;
    char line_[kLineSize];
    size_t length_ = 0;
    bool overflow_ = false;
};
