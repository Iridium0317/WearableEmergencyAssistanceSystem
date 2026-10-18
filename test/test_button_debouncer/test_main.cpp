#include <unity.h>

#include "button_debouncer.h"

namespace {

constexpr uint32_t kDebounce = 30;
constexpr uint32_t kHold = 1500;
constexpr uint32_t kGap = 150;

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

void test_needs_service_only_while_time_matters() {
    ButtonDebouncer b(kDebounce, kHold);
    TEST_ASSERT_FALSE(b.needsService());  // idle: task may sleep forever
    b.onEdge(true, 0);
    TEST_ASSERT_TRUE(b.needsService());   // debounce window open
    drainOne(b, 30);
    TEST_ASSERT_TRUE(b.needsService());   // pressed, waiting for hold
    drainOne(b, 1500);
    TEST_ASSERT_FALSE(b.needsService());  // held already reported
    b.onEdge(false, 2000);
    TEST_ASSERT_TRUE(b.needsService());
    drainOne(b, 2030);
    TEST_ASSERT_FALSE(b.needsService());
}

void test_resync_recovers_missed_release() {
    ButtonDebouncer b(kDebounce, kHold);
    b.onEdge(true, 0);
    drainOne(b, 30);
    // Release edge lost (ring overflow); the pin now reads released.
    b.resync(false, 500);
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 530));
    // Resync to the level we already track is a no-op.
    const uint32_t edges = b.rawEdgeCount();
    b.resync(false, 600);
    TEST_ASSERT_EQUAL_UINT32(edges, b.rawEdgeCount());
}

// --- hold gap tolerance (tremor) -------------------------------------------

void test_gap_within_tolerance_keeps_hold_progress() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 30));
    b.onEdge(false, 700);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 760));  // past 30 ms debounce, inside gap
    b.onEdge(true, 849);                                     // 149 ms lift
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 1499));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1500));  // still timed from 0
    TEST_ASSERT_EQUAL_UINT32(0, b.pressStartMs());
}

void test_gap_at_tolerance_releases_and_resets_hold() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    drainOne(b, 30);
    b.onEdge(false, 700);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 849));
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 850));
    b.onEdge(true, 900);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 930));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 2399));  // hold restarted at 900
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 2400));
}

void test_held_waits_for_contact_during_gap() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    drainOne(b, 30);
    b.onEdge(false, 1450);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 1500));  // threshold reached while lifted
    b.onEdge(true, 1550);
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1550));
}

void test_release_after_held_uses_normal_debounce() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    drainOne(b, 30);
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1500));
    b.onEdge(false, 2000);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 2029));
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 2030));
}

void test_taps_closer_than_gap_merge_into_one_press() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 30));
    b.onEdge(false, 200);
    b.onEdge(true, 300);
    b.onEdge(false, 400);
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 549));
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 550));
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, 3000));
}

void test_gap_tolerance_survives_millis_wraparound() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    const uint32_t start = 0xFFFFFFFFu - 800;
    b.onEdge(true, start);
    drainOne(b, start + 30);
    b.onEdge(false, start + 700);
    b.onEdge(true, start + 840);  // lift straddles the wrap
    TEST_ASSERT_EQUAL(ButtonEvent::None, drainOne(b, start + 1499));
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, start + 1500));
}

// Bench evidence: lift durations from edge timestamps, at 1 ms resolution.
void test_bridged_lifts_are_counted_and_measured() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    drainOne(b, 30);
    b.onEdge(false, 300);
    b.onEdge(true, 310);  // 10 ms: bounce, not counted
    b.onEdge(false, 500);
    b.onEdge(true, 620);  // 120 ms: bridged
    b.onEdge(false, 900);
    b.onEdge(true, 1049);  // 149 ms: bridged
    TEST_ASSERT_EQUAL(ButtonEvent::Held, drainOne(b, 1500));
    TEST_ASSERT_EQUAL_UINT32(2, b.bridgedLifts());
    TEST_ASSERT_EQUAL_UINT32(149, b.maxBridgedLiftMs());
}

void test_lift_that_restarts_hold_is_measured_on_next_press() {
    ButtonDebouncer b(kDebounce, kHold, kGap);
    b.onEdge(true, 0);
    drainOne(b, 30);
    b.onEdge(false, 700);
    TEST_ASSERT_EQUAL(ButtonEvent::Released, drainOne(b, 850));
    b.onEdge(true, 862);  // up for 162 ms
    TEST_ASSERT_EQUAL(ButtonEvent::Pressed, drainOne(b, 892));
    TEST_ASSERT_EQUAL_UINT32(162, b.liftBeforePressMs());
    TEST_ASSERT_EQUAL_UINT32(0, b.bridgedLifts());  // fresh press, counters reset
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
    RUN_TEST(test_needs_service_only_while_time_matters);
    RUN_TEST(test_resync_recovers_missed_release);
    RUN_TEST(test_gap_within_tolerance_keeps_hold_progress);
    RUN_TEST(test_gap_at_tolerance_releases_and_resets_hold);
    RUN_TEST(test_held_waits_for_contact_during_gap);
    RUN_TEST(test_release_after_held_uses_normal_debounce);
    RUN_TEST(test_taps_closer_than_gap_merge_into_one_press);
    RUN_TEST(test_gap_tolerance_survives_millis_wraparound);
    RUN_TEST(test_bridged_lifts_are_counted_and_measured);
    RUN_TEST(test_lift_that_restarts_hold_is_measured_on_next_press);
    return UNITY_END();
}
