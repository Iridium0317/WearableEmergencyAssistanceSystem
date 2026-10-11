#include "button_debouncer.h"

ButtonDebouncer::ButtonDebouncer(uint32_t debounce_ms, uint32_t hold_ms)
    : debounce_ms_(debounce_ms), hold_ms_(hold_ms) {}

void ButtonDebouncer::onEdge(bool pressed, uint32_t timestamp_ms) {
    ++raw_edges_;
    candidate_pressed_ = pressed;
    candidate_since_ms_ = timestamp_ms;
}

ButtonEvent ButtonDebouncer::update(uint32_t now_ms) {
    if (candidate_pressed_ != stable_pressed_ &&
        elapsed(now_ms, candidate_since_ms_, debounce_ms_)) {
        stable_pressed_ = candidate_pressed_;
        if (stable_pressed_) {
            // Hold time counts from the last edge, i.e. when contact settled.
            press_start_ms_ = candidate_since_ms_;
            held_reported_ = false;
            return ButtonEvent::Pressed;
        }
        return ButtonEvent::Released;
    }

    if (stable_pressed_ && !held_reported_ &&
        elapsed(now_ms, press_start_ms_, hold_ms_)) {
        held_reported_ = true;
        return ButtonEvent::Held;
    }
    return ButtonEvent::None;
}

bool ButtonDebouncer::elapsed(uint32_t now, uint32_t since, uint32_t duration) {
    // Unsigned subtraction stays correct across the millis() wrap.
    return now - since >= duration;
}

const char* toString(ButtonEvent event) {
    switch (event) {
        case ButtonEvent::None: return "NONE";
        case ButtonEvent::Pressed: return "PRESSED";
        case ButtonEvent::Released: return "RELEASED";
        case ButtonEvent::Held: return "HELD";
    }
    return "UNKNOWN";
}
