#pragma once

// Debounce + deliberate-hold detection for one button. Pure logic so it can be
// tested on the host with synthetic timestamps.
//
// Input comes from two places with different contexts:
//   - onEdge():  every raw level change, timestamped in the GPIO ISR and
//                forwarded by the button task (this class is NOT ISR-safe;
//                the ISR only enqueues, the task calls onEdge()).
//   - update():  called periodically by the button task to let time pass.
//
// A level is accepted only after it has been stable for debounce_ms. Each
// raw edge restarts that window, so contact bounce never produces events.
// Held fires once per press, hold_ms after the debounced press began.
//
// Hold gap tolerance (for tremor): until Held fires, a release must last
// hold_gap_ms (if longer than debounce_ms) before it counts, so a brief lift
// does not restart the hold. The gap counts toward hold time, but Held only
// fires while the contact is closed. Costs: until Held, Released arrives
// hold_gap_ms after the lift, and two taps less than hold_gap_ms apart merge
// into one press. After Held, releases use debounce_ms again.

#include <stdint.h>

enum class ButtonEvent : uint8_t {
    None,
    Pressed,
    Released,
    Held,
};

class ButtonDebouncer {
public:
    ButtonDebouncer(uint32_t debounce_ms, uint32_t hold_ms, uint32_t hold_gap_ms = 0);

    void onEdge(bool pressed, uint32_t timestamp_ms);

    // Returns at most one event per call; call again until None so that a
    // late update() can still report both Pressed and Held.
    ButtonEvent update(uint32_t now_ms);

    bool isPressed() const { return stable_pressed_; }
    // True while time alone can still produce an event (debounce window open,
    // or pressed but not yet held). The task sleeps indefinitely otherwise.
    bool needsService() const {
        return candidate_pressed_ != stable_pressed_ || (stable_pressed_ && !held_reported_);
    }
    // Re-sync to the actual pin level after edges were lost (ring overflow).
    void resync(bool pressed, uint32_t now_ms) {
        if (pressed != candidate_pressed_) {
            onEdge(pressed, now_ms);
        }
    }
    // Debounced press start; valid after Pressed. Used for latency reporting.
    uint32_t pressStartMs() const { return press_start_ms_; }
    // Raw edges seen since construction; edges per press shows bounce severity.
    uint32_t rawEdgeCount() const { return raw_edges_; }
    // Bench evidence for the gap tolerance, per press (reset on Pressed):
    // lifts that outlasted debounce_ms but were bridged, and the longest one.
    uint32_t bridgedLifts() const { return bridged_lifts_; }
    uint32_t maxBridgedLiftMs() const { return max_bridged_lift_ms_; }
    // Valid after Pressed: how long the button was up before this press,
    // edge to edge. Shows how long a lift that did restart the hold lasted.
    uint32_t liftBeforePressMs() const { return lift_before_press_ms_; }

private:
    static bool elapsed(uint32_t now, uint32_t since, uint32_t duration);

    const uint32_t debounce_ms_;
    const uint32_t hold_ms_;
    const uint32_t hold_gap_ms_;
    bool stable_pressed_ = false;
    bool candidate_pressed_ = false;
    uint32_t candidate_since_ms_ = 0;
    uint32_t press_start_ms_ = 0;
    bool held_reported_ = false;
    uint32_t raw_edges_ = 0;
    uint32_t release_edge_ms_ = 0;
    uint32_t bridged_lifts_ = 0;
    uint32_t max_bridged_lift_ms_ = 0;
    uint32_t lift_before_press_ms_ = 0;
};

const char* toString(ButtonEvent event);
