#pragma once

// Status LED patterns. Pure logic: maps the alert state and the time spent in
// it to an RGB output, so the patterns can be host-tested. States differ in
// rhythm as well as color, so they can be told apart without color vision.
// Prototype patterns, not validated with users.
//
//  State                          Pattern
//  IDLE                           off
//  PENDING                        blue, fast blink (250 ms on / 250 ms off): counting down, can cancel
//  CONFIRMED|ALERT_SENT|RETRY     blue, steady: sending, waiting for the phone
//  ACKNOWLEDGED                   green, steady: the phone accepted the alert
//  CANCELLED                      white, steady
//  ERROR                          red, slow blink (500 ms on / 500 ms off): not delivered, sticky

#include <stdint.h>

#include "state_machine.h"

struct LedColor {
    bool red;
    bool green;
    bool blue;
};

inline bool operator==(const LedColor& a, const LedColor& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}
inline bool operator!=(const LedColor& a, const LedColor& b) { return !(a == b); }

constexpr LedColor kLedOff = {false, false, false};

LedColor statusPattern(State state, uint32_t ms_in_state);
