#include "buttons.h"

#include <Arduino.h>
#include <inttypes.h>

#include "diagnostics.h"

namespace {

MBED_ALIGN(8) unsigned char g_button_stack[3072];

// Button index (0..2) to alert type; the physical order is D2, D3, D4.
AlertType alertFor(uint8_t button) { return alertTypeFromIndex(button + 1); }

}  // namespace

ButtonInput::ButtonInput(AppEventQueue& events)
    : events_(events),
      pin0_(digitalPinToPinName(D2), PullUp),
      pin1_(digitalPinToPinName(D3), PullUp),
      pin2_(digitalPinToPinName(D4), PullUp),
      pins_{&pin0_, &pin1_, &pin2_},
      debouncers_{{kDebounceMs, kHoldMs, kHoldGapMs}, {kDebounceMs, kHoldMs, kHoldGapMs},
                  {kDebounceMs, kHoldMs, kHoldGapMs}},
      // Above the app thread so hold timing stays accurate. Safe because this
      // task never busy-waits and does bounded work per wake-up, so it cannot
      // starve Cordio's bleLoop the way a spinning thread would.
      thread_(osPriorityAboveNormal, sizeof(g_button_stack), g_button_stack, "buttons") {}

void ButtonInput::start() {
    thread_.start(mbed::callback(this, &ButtonInput::run));
    // Interrupts are attached after the task exists, so flags_set() in the
    // ISR always has a valid target.
    pin0_.rise(mbed::callback(this, &ButtonInput::onEdge0));
    pin0_.fall(mbed::callback(this, &ButtonInput::onEdge0));
    pin1_.rise(mbed::callback(this, &ButtonInput::onEdge1));
    pin1_.fall(mbed::callback(this, &ButtonInput::onEdge1));
    pin2_.rise(mbed::callback(this, &ButtonInput::onEdge2));
    pin2_.fall(mbed::callback(this, &ButtonInput::onEdge2));
    started_ = true;
    diag::log("BTN", "ev=started pins=D2,D3,D4 debounce_ms=%" PRIu32 " hold_ms=%" PRIu32 " hold_gap_ms=%" PRIu32,
              kDebounceMs, kHoldMs, kHoldGapMs);
}

void ButtonInput::onEdge(uint8_t button) {
    // ISR: capture, enqueue, wake. Reading the level here (rather than
    // inferring it from rise/fall) keeps the ring correct even if the pin has
    // already bounced again by the time the handler runs.
    const ButtonEdge edge{button, pins_[button]->read() == 0, static_cast<uint32_t>(millis()),
                          static_cast<uint32_t>(micros())};
    isr_edges_.fetch_add(1, std::memory_order_relaxed);
    if (!ring_.push(edge)) {
        overflows_.fetch_add(1, std::memory_order_relaxed);
    }
    thread_.flags_set(kEdgeFlag);
}

void ButtonInput::reportDiagnostics() {
    // 1 = released (pull-up high), 0 = pressed/shorted to GND.
    diag::log("BTN_DIAG", "started=%d d2=%d d3=%d d4=%d isr_edges=%" PRIu32 " overflows=%" PRIu32,
              started_ ? 1 : 0, pin0_.read(), pin1_.read(), pin2_.read(),
              isr_edges_.load(std::memory_order_relaxed), overflows_.load(std::memory_order_relaxed));
}

void ButtonInput::run() {
    for (;;) {
        if (anyNeedsService()) {
            rtos::ThisThread::flags_wait_any_for(kEdgeFlag, std::chrono::milliseconds(kServicePeriodMs));
        } else {
            rtos::ThisThread::flags_wait_any(kEdgeFlag);
        }
        drainEdges();
        const uint32_t now_ms = millis();
        resyncAfterOverflow(now_ms);
        // Pin order is the tie-break: holds that complete in the same wake-up
        // are posted D2, D3, D4, and the state machine keeps the first.
        for (uint8_t b = 0; b < kButtonCount; ++b) {
            serviceButton(b, now_ms);
        }
    }
}

void ButtonInput::drainEdges() {
    ButtonEdge edge;
    while (ring_.pop(edge)) {
        const uint32_t delay_us = static_cast<uint32_t>(micros()) - edge.t_us;
        if (delay_us > max_isr_to_task_us_[edge.button]) {
            max_isr_to_task_us_[edge.button] = delay_us;
        }
        debouncers_[edge.button].onEdge(edge.pressed, edge.t_ms);
    }
}

void ButtonInput::resyncAfterOverflow(uint32_t now_ms) {
    const uint32_t overflows = overflows_.load(std::memory_order_relaxed);
    if (overflows == overflows_seen_) {
        return;
    }
    diag::log("BTN_ERR", "reason=edge_ring_overflow dropped=%" PRIu32, overflows - overflows_seen_);
    overflows_seen_ = overflows;
    // Edges were lost, so the tracked level may be stale: trust the pin.
    for (uint8_t b = 0; b < kButtonCount; ++b) {
        debouncers_[b].resync(pins_[b]->read() == 0, now_ms);
    }
}

void ButtonInput::serviceButton(uint8_t button, uint32_t now_ms) {
    ButtonDebouncer& d = debouncers_[button];
    for (ButtonEvent ev = d.update(now_ms); ev != ButtonEvent::None; ev = d.update(now_ms)) {
        const AlertType alert = alertFor(button);
        switch (ev) {
            case ButtonEvent::Pressed:
                if (!events_.post(Event{EventType::ButtonPressed, now_ms, alert, 0})) {
                    diag::log("BTN_ERR", "reason=queue_full btn=%u", button + 1);
                }
                diag::log("BTN", "ev=PRESSED btn=%u alert=%s debounce_ms=%" PRIu32 " edges=%" PRIu32
                          " isr_to_task_max_us=%" PRIu32 " lift_ms=%" PRIu32,
                          button + 1, toString(alert), now_ms - d.pressStartMs(), d.rawEdgeCount(),
                          max_isr_to_task_us_[button], d.liftBeforePressMs());
                max_isr_to_task_us_[button] = 0;
                break;
            case ButtonEvent::Held: {
                // held_late_ms: how far past the exact hold threshold the task
                // noticed it (bounded by the service period plus scheduling).
                const uint32_t late = now_ms - (d.pressStartMs() + kHoldMs);
                if (!events_.post(Event{EventType::ButtonHeld, now_ms, alert, 0})) {
                    diag::log("BTN_ERR", "reason=queue_full btn=%u", button + 1);
                }
                diag::log("BTN", "ev=HELD btn=%u alert=%s held_late_ms=%" PRIu32 " bridged=%" PRIu32
                          " max_lift_ms=%" PRIu32,
                          button + 1, toString(alert), late, d.bridgedLifts(), d.maxBridgedLiftMs());
                break;
            }
            case ButtonEvent::Released:
                if (!events_.post(Event{EventType::ButtonReleased, now_ms, alert, 0})) {
                    diag::log("BTN_ERR", "reason=queue_full btn=%u", button + 1);
                }
                diag::log("BTN", "ev=RELEASED btn=%u edges=%" PRIu32 " isr_to_task_max_us=%" PRIu32
                          " bridged=%" PRIu32 " max_lift_ms=%" PRIu32,
                          button + 1, d.rawEdgeCount(), max_isr_to_task_us_[button], d.bridgedLifts(),
                          d.maxBridgedLiftMs());
                max_isr_to_task_us_[button] = 0;
                break;
            case ButtonEvent::None:
                break;
        }
    }
}

bool ButtonInput::anyNeedsService() const {
    for (const ButtonDebouncer& d : debouncers_) {
        if (d.needsService()) {
            return true;
        }
    }
    return false;
}
