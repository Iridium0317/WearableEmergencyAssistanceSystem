#include "emergency_controller.h"

#include <Arduino.h>
#include <inttypes.h>

#include "diagnostics.h"

namespace {

// Static stack so starting the thread never allocates from the heap.
MBED_ALIGN(8) unsigned char g_app_stack[4096];

}  // namespace

EmergencyController::EmergencyController(AppEventQueue& events, AlertTransport& transport)
    : events_(events),
      transport_(transport),
      // Above normal so state processing preempts the console and logging.
      thread_(osPriorityAboveNormal, sizeof(g_app_stack), g_app_stack, "app") {}

void EmergencyController::start() {
    thread_.start(mbed::callback(this, &EmergencyController::run));
}

void EmergencyController::run() {
    diag::log("SM", "state=%s transport=%s", toString(machine_.state()), transport_.name());
    for (;;) {
        Event event;
        if (events_.wait(event, kTickPeriodMs)) {
            dispatch(event);
        }
        dispatch(Event{EventType::Tick, millis(), AlertType::None, 0});
    }
}

void EmergencyController::dispatch(const Event& event) {
    const Result result = machine_.handle(event);
    const uint32_t now = millis();
    report(event, result, now);
    if (result.outcome == Outcome::Transitioned) {
        execute(result.action, now);
    }
}

void EmergencyController::report(const Event& event, const Result& result, uint32_t now) const {
    // lat_ms = time from the event being produced to it being processed.
    const uint32_t latency = now - event.timestamp_ms;
    if (result.outcome == Outcome::Transitioned) {
        diag::log("SM", "from=%s to=%s ev=%s alert=%s id=%u attempt=%u lat_ms=%" PRIu32 " reason=%s",
                  toString(result.from), toString(result.to), toString(event.type),
                  toString(machine_.alertType()), machine_.alertId(), machine_.attempts(),
                  latency, result.reason ? result.reason : "-");
    } else if (result.outcome == Outcome::Rejected) {
        diag::log("SM_REJECT", "state=%s ev=%s ev_id=%u reason=%s",
                  toString(result.from), toString(event.type), event.alert_id, result.reason);
    }
}

void EmergencyController::execute(Action action, uint32_t now) {
    const uint16_t id = machine_.alertId();
    switch (action) {
        case Action::None:
            return;
        case Action::SendAlert: {
            const bool ok = transport_.sendAlert(id, machine_.alertType(), machine_.attempts());
            // Fed straight back in rather than queued: the result belongs to
            // this transition and must not be reordered behind other events.
            dispatch(Event{ok ? EventType::AlertSendOk : EventType::AlertSendFailed, now,
                           AlertType::None, id});
            return;
        }
        case Action::SendCancel: {
            // Best effort: the user already cancelled locally either way.
            const bool ok = transport_.sendCancel(id);
            if (!ok) {
                diag::log("ERR", "op=send_cancel id=%u reason=link_unavailable", id);
            }
            return;
        }
    }
}
