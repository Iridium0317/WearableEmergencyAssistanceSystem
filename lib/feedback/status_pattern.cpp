#include "status_pattern.h"

namespace {

constexpr uint32_t kPendingHalfPeriodMs = 250;
constexpr uint32_t kErrorHalfPeriodMs = 500;

constexpr LedColor kBlue = {false, false, true};
constexpr LedColor kGreen = {false, true, false};
constexpr LedColor kRed = {true, false, false};
constexpr LedColor kWhite = {true, true, true};

// On for the first half of each period, starting on as the state is entered.
LedColor blink(LedColor color, uint32_t ms_in_state, uint32_t half_period_ms) {
    return (ms_in_state / half_period_ms) % 2 == 0 ? color : kLedOff;
}

}  // namespace

LedColor statusPattern(State state, uint32_t ms_in_state) {
    switch (state) {
        case State::Idle: return kLedOff;
        case State::Pending: return blink(kBlue, ms_in_state, kPendingHalfPeriodMs);
        case State::Confirmed:
        case State::AlertSent:
        case State::Retry: return kBlue;
        case State::Acknowledged: return kGreen;
        case State::Cancelled: return kWhite;
        case State::Error: return blink(kRed, ms_in_state, kErrorHalfPeriodMs);
    }
    return kLedOff;
}
