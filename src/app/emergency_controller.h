#pragma once

// Owns the StateMachine and runs it on a dedicated RTOS thread. This is the
// only thread that touches the state machine, so it needs no locking; every
// other context talks to it through AppEventQueue.

#include <mbed.h>

#include "alert_transport.h"
#include "event_queue.h"
#include "state_machine.h"

class EmergencyController {
public:
    EmergencyController(AppEventQueue& events, AlertTransport& transport);

    void start();

private:
    // Upper bound on timeout lateness: deadlines are checked on every event
    // and at least this often when the queue is idle.
    static constexpr uint32_t kTickPeriodMs = 10;

    void run();
    void dispatch(const Event& event);
    void report(const Event& event, const Result& result, uint32_t now) const;
    void execute(Action action, uint32_t now);

    AppEventQueue& events_;
    AlertTransport& transport_;
    StateMachine machine_;
    rtos::Thread thread_;
};
