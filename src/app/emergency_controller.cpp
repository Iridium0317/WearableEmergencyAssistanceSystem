#include "emergency_controller.h"

#include <Arduino.h>
#include <inttypes.h>

#include "diagnostics.h"

namespace {

// Static stack so starting the thread never allocates from the heap. Sized
// for ArduinoBLE's poll path (ATT handling uses MTU-sized stack buffers).
MBED_ALIGN(8) unsigned char g_app_stack[8192];

}  // namespace

EmergencyController::EmergencyController(AppEventQueue& events, AlertTransport& transport, StatusLed& led)
    : events_(events),
      transport_(transport),
      led_(led),
      // Must stay BELOW Cordio's bleLoop thread (osPriorityNormal). ArduinoBLE
      // busy-waits up to 1 s for each HCI command response, and that response
      // is produced by bleLoop; at AboveNormal this thread starved bleLoop and
      // BLE.begin() timed out (observed on hardware: op=begin init_failed).
      thread_(osPriorityBelowNormal, sizeof(g_app_stack), g_app_stack, "app") {}

void EmergencyController::start() {
    thread_.start(mbed::callback(this, &EmergencyController::run));
}

void EmergencyController::run() {
    // begin() runs here, not in setup(): the transport may only be touched
    // from this thread (ArduinoBLE is not thread-safe).
    const bool link_ok = transport_.begin();
    diag::log("SM", "state=%s transport=%s link_ok=%d", toString(machine_.state()), transport_.name(),
              link_ok ? 1 : 0);
    publishState();
    for (;;) {
        Event event;
        if (events_.wait(event, kTickPeriodMs)) {
            dispatch(event);
        }
        transport_.poll();
        // poll() can block (ArduinoBLE busy-waits on HCI). Handle whatever
        // queued up meanwhile before time advances, so a press timestamped
        // before a deadline is not beaten by the Tick that expires it.
        // Bounded so a flood of events cannot starve the Tick.
        for (uint32_t i = 0; i < AppEventQueue::kCapacity && events_.wait(event, 0); ++i) {
            dispatch(event);
        }
        const uint32_t now = millis();
        dispatch(Event{EventType::Tick, now, AlertType::None, 0});
        led_.show(statusPattern(machine_.state(), now - state_since_ms_));
    }
}

void EmergencyController::publishState() {
    transport_.publishState(machine_.state(), machine_.alertId(), machine_.alertType(), machine_.attempts());
}

void EmergencyController::dispatch(const Event& event) {
    const Result result = machine_.handle(event);
    const uint32_t now = millis();
    report(event, result, now);
    if (result.outcome == Outcome::Transitioned) {
        state_since_ms_ = now;
        publishState();
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
    } else if (result.outcome == Outcome::Unchanged && result.reason != nullptr) {
        diag::log("SM_INFO", "state=%s ev=%s alert=%s id=%u reason=%s", toString(result.from),
                  toString(event.type), toString(machine_.alertType()), machine_.alertId(), result.reason);
    } else if (result.outcome == Outcome::Rejected) {
        diag::log("SM_REJECT", "state=%s ev=%s ev_id=%u reason=%s",
                  toString(result.from), toString(event.type), event.alert_id, result.reason);
        if (event.type == EventType::HostAck && result.from == State::Error &&
            event.alert_id == machine_.alertId()) {
            diag::log("ACK", "late_ack_after_error=1 id=%u alert=%s", event.alert_id,
                      toString(machine_.alertType()));
        }
    }
}

void EmergencyController::execute(Action action, uint32_t now) {
    const uint16_t id = machine_.alertId();
    switch (action) {
        case Action::None:
            return;
        case Action::SendAlert: {
            const SendResult sent = transport_.sendAlert(id, machine_.alertType(), machine_.attempts());
            const EventType result = sent == SendResult::Sent       ? EventType::AlertSendOk
                                     : sent == SendResult::NotReady ? EventType::AlertSendNotReady
                                                                    : EventType::AlertSendFailed;
            // Fed straight back in rather than queued: the result belongs to
            // this transition and must not be reordered behind other events.
            dispatch(Event{result, now, AlertType::None, id});
            return;
        }
        case Action::SendCancel: {
            // Best effort, sent once: the user already cancelled locally
            // either way. There is no CANCEL ack in the protocol, so even a
            // successful send does not mean the host has cancelled.
            const bool ok = transport_.sendCancel(id, machine_.alertType());
            if (!ok) {
                diag::log("ERR", "op=send_cancel id=%u reason=link_unavailable cancel_delivery=unknown", id);
            }
            return;
        }
    }
}
