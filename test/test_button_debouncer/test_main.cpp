#include <unity.h>

#include "button_debouncer.h"

namespace {

constexpr uint32_t kDebounce = 30;
constexpr uint32_t kHold = 1500;

ButtonEvent drainOne(ButtonDebouncer& b, uint32_t now) { return b.update(now); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_clean_press_reports_after_debounce_window() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 100);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 129));
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 130));
    TEST_ASSERT_TRUE(b.isPressed());
    TEST_ASSERT_EQUAL_UINT32(100, b.pressStartMs());
}

void test_bounce_produces_single_press() {
    ButtonDebouncer b(kDebounce, kHold);
    // Contact chatter: press/release/press/release/press within 8 ms.
    const uint32_t times[] = {0, 2, 5, 7, 8};
    for (uint32_t i = 0; i < 5; ++i) {
        b.onEdge(i % 2 == 0, times[i]);
        TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, times[i]));
    }
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 37));
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 38));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 39));
    TEST_ASSERT_EQUAL_UINT32(5, b.rawEdgeCount());
}

void test_glitch_shorter_than_debounce_is_ignored() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    b.onEdge(false, 10);  // back to the stable level before 30 ms
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 100));
    TEST_ASSERT_FALSE(b.isPressed());
}

void test_held_fires_exactly_at_threshold_once() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 30));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 1499));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1500));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 5000));
}

void test_short_press_never_holds() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 30));
    b.onEdge(false, 800);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 829));
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 830));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 3000));
}

void test_late_update_reports_press_then_hold() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 2000));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 2000));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 2000));
}

void test_second_press_can_hold_again() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    drainOne(b, 30);
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1500));
    b.onEdge(false, 2000);
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 2030));
    b.onEdge(true, 3000);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 3030));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 4500));
}

void test_timing_survives_millis_wraparound() {
    ButtonDebouncer b(kDebounce, kHold);
    const uint32_t start = 0xFFFFFFFFu - 10;
    b.onEdge(true, start);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, start + 30));  // wraps
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, start + 1499));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, start + 1500));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_clean_press_reports_after_debounce_window);
    RUN_TEST(test_bounce_produces_single_press);
    RUN_TEST(test_glitch_shorter_than_debounce_is_ignored);
    RUN_TEST(test_held_fires_exactly_at_threshold_once);
    RUN_TEST(test_short_press_never_holds);
    RUN_TEST(test_late_update_reports_press_then_hold);
    RUN_TEST(test_second_press_can_hold_again);
    RUN_TEST(test_timing_survives_millis_wraparound);
    return UNITY_END();
}
