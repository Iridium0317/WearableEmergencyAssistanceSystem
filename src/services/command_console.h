#pragma once

// Line-based serial command console used by the bench tool to inject events.
//
//   ping              -> CMD_OK ping   (liveness check)
//   reboot            -> CMD_OK, then system reset (bench starts from IDLE)
//   hold <1-3>        -> ButtonPressed + ButtonHeld for that alert type
//   press             -> ButtonPressed (the cancel gesture)
//   ack <id>          -> HostAck for alert <id>
//   link up|down      -> simulated host link state + LinkUp/LinkDown event
//
// Every line gets exactly one CMD_OK or CMD_ERR reply; malformed input is
// always reported, never dropped silently. Runs in the Arduino loop thread.

#include <stddef.h>

#include "event_queue.h"
#include "simulated_link.h"

class CommandConsole {
public:
    CommandConsole(AppEventQueue& events, SimulatedLink& link);

    // Non-blocking: consumes whatever serial bytes are available.
    void poll();

private:
    static constexpr size_t kLineSize = 48;

    void execute(char* line);
    bool post(EventType type, AlertType alert = AlertType::None, uint16_t id = 0);

    AppEventQueue& events_;
    SimulatedLink& link_;
    char line_[kLineSize];
    size_t length_ = 0;
    bool overflow_ = false;
};
