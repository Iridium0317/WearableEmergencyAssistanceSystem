#include <unity.h>

#include "status_pattern.h"

namespace {

constexpr LedColor kBlue = {false, false, true};
constexpr LedColor kGreen = {false, true, false};
constexpr LedColor kRed = {true, false, false};
constexpr LedColor kWhite = {true, true, true};

void assertColor(const LedColor& expected, const LedColor& actual) {
    TEST_ASSERT_EQUAL(expected.red, actual.red);
    TEST_ASSERT_EQUAL(expected.green, actual.green);
    TEST_ASSERT_EQUAL(expected.blue, actual.blue);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_idle_is_off() { assertColor(kLedOff, statusPattern(State::Idle, 12345)); }

void test_pending_blinks_blue_fast_starting_on() {
    assertColor(kBlue, statusPattern(State::Pending, 0));
    assertColor(kBlue, statusPattern(State::Pending, 249));
    assertColor(kLedOff, statusPattern(State::Pending, 250));
    assertColor(kLedOff, statusPattern(State::Pending, 499));
    assertColor(kBlue, statusPattern(State::Pending, 500));
}

void test_sending_states_are_steady_blue() {
    const State sending[] = {State::Confirmed, State::AlertSent, State::Retry};
    for (const State s : sending) {
        assertColor(kBlue, statusPattern(s, 0));
        assertColor(kBlue, statusPattern(s, 250));
        assertColor(kBlue, statusPattern(s, 4999));
    }
}

void test_acknowledged_is_steady_green_and_cancelled_steady_white() {
    assertColor(kGreen, statusPattern(State::Acknowledged, 0));
    assertColor(kGreen, statusPattern(State::Acknowledged, 2999));
    assertColor(kWhite, statusPattern(State::Cancelled, 0));
    assertColor(kWhite, statusPattern(State::Cancelled, 999));
}

void test_error_blinks_red_slowly_forever() {
    assertColor(kRed, statusPattern(State::Error, 0));
    assertColor(kRed, statusPattern(State::Error, 499));
    assertColor(kLedOff, statusPattern(State::Error, 500));
    assertColor(kRed, statusPattern(State::Error, 1000));
    // Sticky: still blinking after an hour (no wrap within a realistic span).
    assertColor(kRed, statusPattern(State::Error, 3600000));
}

// Rhythm alone separates the states that share a color, so the pattern can be
// read without color vision: PENDING blinks, sending is steady.
void test_pending_and_sending_differ_by_rhythm() {
    TEST_ASSERT_TRUE(statusPattern(State::Pending, 300) != statusPattern(State::Confirmed, 300));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_is_off);
    RUN_TEST(test_pending_blinks_blue_fast_starting_on);
    RUN_TEST(test_sending_states_are_steady_blue);
    RUN_TEST(test_acknowledged_is_steady_green_and_cancelled_steady_white);
    RUN_TEST(test_error_blinks_red_slowly_forever);
    RUN_TEST(test_pending_and_sending_differ_by_rhythm);
    return UNITY_END();
}
