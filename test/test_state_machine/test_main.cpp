#include <unity.h>

#include "state_machine.h"

namespace {

// The original scenarios use the earlier timing (3 s window timed from the
// hold, no release restart) so their numbers stay readable. The 5 s
// release-timed defaults have their own tests below.
constexpr Timing kLegacy = {3000, 5000, 2000, 3, 3000, 1000, 300, false, 20000};

Event ev(EventType type, uint32_t t, AlertType alert = AlertType::None, uint16_t id = 0) {
    return Event{type, t, alert, id};
}

Event held(uint32_t t, AlertType a = AlertType::Emergency) { return ev(EventType::ButtonHeld, t, a); }
Event pressed(uint32_t t, AlertType a = AlertType::None) { return ev(EventType::ButtonPressed, t, a); }
Event released(uint32_t t, AlertType a) { return ev(EventType::ButtonReleased, t, a); }
Event tick(uint32_t t) { return ev(EventType::Tick, t); }
Event sendOk(uint32_t t, uint16_t id) { return ev(EventType::AlertSendOk, t, AlertType::None, id); }
Event sendFail(uint32_t t, uint16_t id) { return ev(EventType::AlertSendFailed, t, AlertType::None, id); }
Event ack(uint32_t t, uint16_t id) { return ev(EventType::HostAck, t, AlertType::None, id); }
Event notReady(uint32_t t, uint16_t id) { return ev(EventType::AlertSendNotReady, t, AlertType::None, id); }
Event linkUp(uint32_t t) { return ev(EventType::LinkUp, t); }
Event linkDown(uint32_t t) { return ev(EventType::LinkDown, t); }

void assertTransition(const Result& r, State from, State to, Action action) {
    TEST_ASSERT_EQUAL(Outcome::Transitioned, r.outcome);
    TEST_ASSERT_EQUAL_STRING(toString(from), toString(r.from));
    TEST_ASSERT_EQUAL_STRING(toString(to), toString(r.to));
    TEST_ASSERT_EQUAL_STRING(toString(action), toString(r.action));
}

void assertRejected(const Result& r, StateMachine& sm, State expected_state) {
    TEST_ASSERT_EQUAL(Outcome::Rejected, r.outcome);
    TEST_ASSERT_NOT_NULL(r.reason);
    TEST_ASSERT_EQUAL_STRING(toString(expected_state), toString(sm.state()));
}

// Drives a fresh machine to ALERT_SENT at t=3000 (send ok at 3000).
void driveToAlertSent(StateMachine& sm) {
    sm.handle(held(0));
    sm.handle(tick(3000));
    sm.handle(sendOk(3000, sm.alertId()));
    TEST_ASSERT_EQUAL_STRING("ALERT_SENT", toString(sm.state()));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_happy_path_hold_send_ack_idle() {
    StateMachine sm(kLegacy);
    assertTransition(sm.handle(held(0, AlertType::Medical)), State::Idle, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_UINT16(1, sm.alertId());
    TEST_ASSERT_EQUAL_STRING("MEDICAL", toString(sm.alertType()));

    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(2999)).outcome);
    assertTransition(sm.handle(tick(3000)), State::Pending, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());

    assertTransition(sm.handle(sendOk(3005, 1)), State::Confirmed, State::AlertSent, Action::None);
    assertTransition(sm.handle(ack(3100, 1)), State::AlertSent, State::Acknowledged, Action::None);

    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(6099)).outcome);
    assertTransition(sm.handle(tick(6100)), State::Acknowledged, State::Idle, Action::None);
}

void test_cancel_during_pending_sends_nothing() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    const Result r = sm.handle(pressed(1000));
    assertTransition(r, State::Pending, State::Cancelled, Action::None);
    TEST_ASSERT_EQUAL_STRING("user_cancel", r.reason);
    assertTransition(sm.handle(tick(2000)), State::Cancelled, State::Idle, Action::None);
}

void test_cancel_after_send_notifies_host() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);
    assertTransition(sm.handle(pressed(3500)), State::AlertSent, State::Cancelled, Action::SendCancel);
}

void test_ack_timeout_retries_then_errors_after_max_attempts() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // attempt 1 sent at 3000

    const Result t1 = sm.handle(tick(8000));  // 5 s ack timeout
    assertTransition(t1, State::AlertSent, State::Retry, Action::None);
    TEST_ASSERT_EQUAL_STRING("ack_timeout", t1.reason);

    assertTransition(sm.handle(tick(10000)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(2, sm.attempts());
    sm.handle(sendOk(10000, sm.alertId()));
    sm.handle(tick(15000));
    assertTransition(sm.handle(tick(17000)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(3, sm.attempts());
    sm.handle(sendOk(17000, sm.alertId()));

    const Result last = sm.handle(tick(22000));
    assertTransition(last, State::AlertSent, State::Error, Action::None);
    TEST_ASSERT_EQUAL_STRING("ack_timeout", last.reason);
}

void test_send_failure_is_bounded() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    uint32_t t = 3000;
    for (int attempt = 1; attempt < kDefaultTiming.max_attempts; ++attempt) {
        assertTransition(sm.handle(sendFail(t, sm.alertId())), State::Confirmed, State::Retry, Action::None);
        t += kDefaultTiming.retry_backoff_ms;
        assertTransition(sm.handle(tick(t)), State::Retry, State::Confirmed, Action::SendAlert);
    }
    assertTransition(sm.handle(sendFail(t, sm.alertId())), State::Confirmed, State::Error, Action::None);
    TEST_ASSERT_EQUAL_UINT8(kDefaultTiming.max_attempts, sm.attempts());
}

void test_link_down_while_waiting_for_ack_retries() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);
    const Result r = sm.handle(ev(EventType::LinkDown, 3500));
    assertTransition(r, State::AlertSent, State::Retry, Action::None);
    TEST_ASSERT_EQUAL_STRING("link_down", r.reason);
}

void test_late_ack_during_retry_is_accepted() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);
    sm.handle(tick(8000));  // -> RETRY
    assertTransition(sm.handle(ack(8500, sm.alertId())), State::Retry, State::Acknowledged, Action::None);
}

void test_ack_with_wrong_id_is_rejected() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);
    assertRejected(sm.handle(ack(3100, sm.alertId() + 1)), sm, State::AlertSent);
}

void test_ack_without_alert_is_rejected() {
    StateMachine sm(kLegacy);
    assertRejected(sm.handle(ack(10, 1)), sm, State::Idle);
}

void test_stale_send_result_is_rejected() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    assertRejected(sm.handle(sendOk(3001, sm.alertId() + 7)), sm, State::Confirmed);
}

void test_second_hold_during_alert_is_rejected() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    assertRejected(sm.handle(held(100, AlertType::Family)), sm, State::Pending);
    TEST_ASSERT_EQUAL_STRING("EMERGENCY", toString(sm.alertType()));
}

void test_invalid_alert_type_is_rejected() {
    StateMachine sm(kLegacy);
    assertRejected(sm.handle(held(0, AlertType::None)), sm, State::Idle);
}

void test_error_is_sticky_until_new_hold() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    for (int i = 0; i < kDefaultTiming.max_attempts; ++i) {
        sm.handle(sendFail(3000 + i * 2000, sm.alertId()));
        sm.handle(tick(3000 + (i + 1) * 2000));
    }
    TEST_ASSERT_EQUAL_STRING("ERROR", toString(sm.state()));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(1000000)).outcome);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(pressed(1000001)).outcome);

    const uint16_t old_id = sm.alertId();
    assertTransition(sm.handle(held(1000002)), State::Error, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_UINT16(old_id + 1, sm.alertId());
    TEST_ASSERT_EQUAL_UINT8(0, sm.attempts());
}

void test_new_hold_during_ack_display_starts_new_alert() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);
    sm.handle(ack(3100, sm.alertId()));
    assertTransition(sm.handle(held(3500, AlertType::Family)), State::Acknowledged, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_UINT16(2, sm.alertId());
}

// Cancel and a new alert are separate actions. A wrong-button hold is
// corrected by pressing another button (cancels), releasing it, then holding
// the intended one. The cancelling press's own hold must not start an alert,
// even after CANCELLED has timed out to IDLE.
void test_cancel_press_hold_does_not_start_new_alert() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Family));
    assertTransition(sm.handle(pressed(1000, AlertType::Emergency)), State::Pending, State::Cancelled,
                     Action::None);
    assertTransition(sm.handle(tick(2000)), State::Cancelled, State::Idle, Action::None);
    const Result r = sm.handle(held(2500, AlertType::Emergency));  // same press, still down
    assertRejected(r, sm, State::Idle);
    TEST_ASSERT_EQUAL_STRING("hold_after_consumed_press", r.reason);

    sm.handle(released(3000, AlertType::Emergency));
    sm.handle(pressed(4000, AlertType::Emergency));
    assertTransition(sm.handle(held(5500, AlertType::Emergency)), State::Idle, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_STRING("EMERGENCY", toString(sm.alertType()));
    TEST_ASSERT_EQUAL_UINT16(2, sm.alertId());
}

void test_cancel_press_hold_after_send_does_not_start_new_alert() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // alert 1
    assertTransition(sm.handle(pressed(3500, AlertType::Medical)), State::AlertSent, State::Cancelled,
                     Action::SendCancel);
    assertRejected(sm.handle(held(5000, AlertType::Medical)), sm, State::Cancelled);

    sm.handle(released(5200, AlertType::Medical));
    sm.handle(tick(5200));  // CANCELLED display over -> IDLE
    sm.handle(pressed(6000, AlertType::Medical));
    assertTransition(sm.handle(held(7500, AlertType::Medical)), State::Idle, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_STRING("MEDICAL", toString(sm.alertType()));
    TEST_ASSERT_EQUAL_UINT16(2, sm.alertId());
}

// A press rejected by the lockout is consumed too: if the alert fails while
// it is still down, its hold must not start a new alert from ERROR.
void test_lockout_rejected_press_hold_does_not_start_alert_from_error() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Emergency));
    sm.handle(tick(3000));
    sm.handle(sendFail(3000, 1));
    sm.handle(tick(5000));
    sm.handle(sendFail(5000, 1));
    sm.handle(released(6800, AlertType::Emergency));
    assertRejected(sm.handle(pressed(6900, AlertType::Emergency)), sm, State::Retry);  // lockout
    sm.handle(tick(7000));
    assertTransition(sm.handle(sendFail(7000, 1)), State::Confirmed, State::Error, Action::None);
    assertRejected(sm.handle(held(8400, AlertType::Emergency)), sm, State::Error);
    TEST_ASSERT_EQUAL_UINT16(1, sm.alertId());
}

// If a release event is lost, the next press outside an alert proves the
// button was released, so it clears the stale mark.
void test_fresh_press_outside_alert_clears_consumed_mark() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Family));
    sm.handle(pressed(1000, AlertType::Emergency));  // cancels; its release is never seen
    sm.handle(tick(2000));                           // IDLE
    sm.handle(pressed(5000, AlertType::Emergency));
    assertTransition(sm.handle(held(6500, AlertType::Emergency)), State::Idle, State::Pending, Action::None);
}

// A press during the CANCELLED display is consumed too, so its hold cannot
// start an alert; a press once back in IDLE can (the ERROR case is
// test_error_is_sticky_until_new_hold).
void test_press_during_cancelled_display_is_consumed() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Family));
    sm.handle(pressed(1000, AlertType::Medical));  // cancel
    sm.handle(released(1100, AlertType::Medical));
    sm.handle(pressed(1200, AlertType::Medical));  // during CANCELLED display
    assertRejected(sm.handle(held(2700, AlertType::Medical)), sm, State::Cancelled);

    sm.handle(tick(2000));  // IDLE
    sm.handle(released(2800, AlertType::Medical));
    sm.handle(pressed(3000, AlertType::Medical));
    assertTransition(sm.handle(held(4500, AlertType::Medical)), State::Idle, State::Pending, Action::None);
}

// Two fingers on two buttons in the same instant: the first press cancels,
// the second lands in CANCELLED. Holding both must not start a new alert.
void test_simultaneous_cancel_presses_second_hold_does_not_start_alert() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Family));
    assertTransition(sm.handle(pressed(1000, AlertType::Emergency)), State::Pending, State::Cancelled,
                     Action::None);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(pressed(1000, AlertType::Medical)).outcome);
    sm.handle(tick(2000));  // IDLE
    assertRejected(sm.handle(held(2500, AlertType::Emergency)), sm, State::Idle);
    assertRejected(sm.handle(held(2500, AlertType::Medical)), sm, State::Idle);
}

// --- default timing: 5 s window timed from the own button's release ---------

void test_default_window_is_5s_from_release() {
    TEST_ASSERT_EQUAL_UINT32(5000, kDefaultTiming.pending_ms);
    TEST_ASSERT_TRUE(kDefaultTiming.pending_from_release);
    StateMachine sm;
    sm.handle(held(1500, AlertType::Emergency));
    const Result r = sm.handle(released(4000, AlertType::Emergency));  // held 2.5 s past HELD
    TEST_ASSERT_EQUAL(Outcome::Unchanged, r.outcome);
    TEST_ASSERT_EQUAL_STRING("cancel_window_started", r.reason);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(8999)).outcome);
    assertTransition(sm.handle(tick(9000)), State::Pending, State::Confirmed, Action::SendAlert);
}

// Never released (e.g. lying on the button): the alert still goes out,
// pending_ms after HELD.
void test_window_falls_back_to_hold_when_never_released() {
    StateMachine sm;
    sm.handle(held(1500, AlertType::Emergency));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(6499)).outcome);
    assertTransition(sm.handle(tick(6500)), State::Pending, State::Confirmed, Action::SendAlert);
}

// A release that arrives after the fallback deadline (before the Tick that
// would send) must not extend the window.
void test_release_after_fallback_deadline_does_not_extend() {
    StateMachine sm;
    sm.handle(held(1500, AlertType::Emergency));
    const Result r = sm.handle(released(6600, AlertType::Emergency));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, r.outcome);
    TEST_ASSERT_NULL(r.reason);
    assertTransition(sm.handle(tick(6600)), State::Pending, State::Confirmed, Action::SendAlert);
}

// Only the first release restarts the window; tremor taps on the own button
// afterwards are rejected by the lockout and neither cancel nor delay the send.
void test_tremor_taps_after_release_neither_cancel_nor_delay() {
    StateMachine sm;
    sm.handle(held(0, AlertType::Emergency));
    sm.handle(released(100, AlertType::Emergency));  // window -> 5100
    for (uint32_t t = 250; t < 5000; t += 250) {      // press 150 ms after each release
        assertRejected(sm.handle(pressed(t, AlertType::Emergency)), sm, State::Pending);
        TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(released(t + 100, AlertType::Emergency)).outcome);
    }
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(5099)).outcome);
    assertTransition(sm.handle(tick(5100)), State::Pending, State::Confirmed, Action::SendAlert);
}

void test_other_button_release_does_not_start_window() {
    StateMachine sm;
    sm.handle(held(1500, AlertType::Emergency));
    TEST_ASSERT_NULL(sm.handle(released(2000, AlertType::Medical)).reason);
    assertTransition(sm.handle(tick(6500)), State::Pending, State::Confirmed, Action::SendAlert);
}

// Same boundaries for both candidate windows, timed from the release.
void test_cancel_boundaries_for_3s_and_5s_windows() {
    const uint32_t windows[] = {3000, 5000};
    for (const uint32_t w : windows) {
        Timing timing = kDefaultTiming;
        timing.pending_ms = w;
        const uint32_t release = 2000;

        StateMachine cancel_in_time(timing);
        cancel_in_time.handle(held(1500, AlertType::Emergency));
        cancel_in_time.handle(released(release, AlertType::Emergency));
        TEST_ASSERT_EQUAL(Outcome::Unchanged, cancel_in_time.handle(tick(release + w - 1)).outcome);
        assertTransition(cancel_in_time.handle(pressed(release + w - 1, AlertType::Family)), State::Pending,
                         State::Cancelled, Action::None);

        StateMachine cancel_late(timing);
        cancel_late.handle(held(1500, AlertType::Emergency));
        cancel_late.handle(released(release, AlertType::Emergency));
        assertTransition(cancel_late.handle(tick(release + w)), State::Pending, State::Confirmed,
                         Action::SendAlert);
        cancel_late.handle(sendOk(release + w, cancel_late.alertId()));
        assertTransition(cancel_late.handle(pressed(release + w + 1, AlertType::Family)), State::AlertSent,
                         State::Cancelled, Action::SendCancel);
    }
}

void test_release_timed_window_survives_millis_wraparound() {
    StateMachine sm;
    const uint32_t release = 0xFFFFFFFFu - 1000;
    sm.handle(held(release - 2000, AlertType::Medical));
    sm.handle(released(release, AlertType::Medical));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(release + 4999)).outcome);  // wrapped
    assertTransition(sm.handle(tick(release + 5000)), State::Pending, State::Confirmed, Action::SendAlert);
}

// Two holds in the same instant: the first event wins and keeps its type.
// The button task posts same-tick events in pin order D2, D3, D4, so on a tie
// Emergency beats Medical beats Family.
void test_simultaneous_holds_first_event_wins() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    assertRejected(sm.handle(held(1500, AlertType::Medical)), sm, State::Pending);
    TEST_ASSERT_EQUAL_STRING("EMERGENCY", toString(sm.alertType()));
    TEST_ASSERT_EQUAL_UINT16(1, sm.alertId());
}

// Wobble guard: a finger slipping off the held button and back on (observed
// on hardware: a single clean 48 ms open) must not cancel the alert.
void test_held_button_repress_within_lockout_does_not_cancel() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(released(2000, AlertType::Emergency)).outcome);
    assertRejected(sm.handle(pressed(2048, AlertType::Emergency)), sm, State::Pending);
    assertRejected(sm.handle(pressed(2299, AlertType::Emergency)), sm, State::Pending);
    // The alert carries on as if nothing happened.
    assertTransition(sm.handle(tick(4500)), State::Pending, State::Confirmed, Action::SendAlert);
}

void test_held_button_repress_after_lockout_cancels() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    sm.handle(released(2000, AlertType::Emergency));
    assertTransition(sm.handle(pressed(2300, AlertType::Emergency)), State::Pending, State::Cancelled,
                     Action::None);
}

void test_other_button_cancels_during_lockout() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    sm.handle(released(2000, AlertType::Emergency));
    assertTransition(sm.handle(pressed(2010, AlertType::Medical)), State::Pending, State::Cancelled,
                     Action::None);
}

void test_held_button_repress_within_lockout_does_not_cancel_in_flight() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // Emergency, alert 1
    sm.handle(released(3200, AlertType::Emergency));
    assertRejected(sm.handle(pressed(3250, AlertType::Emergency)), sm, State::AlertSent);
    assertTransition(sm.handle(pressed(3260, AlertType::Family)), State::AlertSent, State::Cancelled,
                     Action::SendCancel);
}

// Releasing a button other than the alert's own does not arm the lockout.
void test_other_button_release_does_not_arm_lockout() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    sm.handle(released(2000, AlertType::Medical));
    assertTransition(sm.handle(pressed(2050, AlertType::Emergency)), State::Pending, State::Cancelled,
                     Action::None);
}

void test_cancel_lockout_survives_millis_wraparound() {
    StateMachine sm(kLegacy);
    const uint32_t release = 0xFFFFFFFFu - 100;
    sm.handle(held(release - 500, AlertType::Medical));
    sm.handle(released(release, AlertType::Medical));
    assertRejected(sm.handle(pressed(release + 200, AlertType::Medical)), sm, State::Pending);  // wrapped
    assertTransition(sm.handle(pressed(release + 300, AlertType::Medical)), State::Pending, State::Cancelled,
                     Action::None);
}

// A press rejected by the lockout is dropped, not deferred: the lockout
// expiring later must not turn it into a cancel.
void test_lockout_rejected_press_is_not_replayed_after_lockout() {
    StateMachine sm(kLegacy);
    sm.handle(held(1500, AlertType::Emergency));
    sm.handle(released(2000, AlertType::Emergency));
    assertRejected(sm.handle(pressed(2100, AlertType::Emergency)), sm, State::Pending);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(2400)).outcome);
    TEST_ASSERT_EQUAL_STRING("PENDING", toString(sm.state()));
    assertTransition(sm.handle(tick(4500)), State::Pending, State::Confirmed, Action::SendAlert);
}

// Cancel racing the retry timer: once CANCELLED, the old retry deadline is
// gone and late send results for that alert are stale.
void test_cancel_during_retry_kills_pending_resend() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    assertTransition(sm.handle(sendFail(3000, 1)), State::Confirmed, State::Retry, Action::None);  // resend due 5000
    assertTransition(sm.handle(pressed(4999, AlertType::Family)), State::Retry, State::Cancelled,
                     Action::SendCancel);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(5000)).outcome);
    TEST_ASSERT_EQUAL_STRING("CANCELLED", toString(sm.state()));
    assertRejected(sm.handle(sendOk(5001, 1)), sm, State::Cancelled);
    assertTransition(sm.handle(tick(5999)), State::Cancelled, State::Idle, Action::None);
    TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());
}

void test_late_ack_after_error_is_rejected_and_keeps_alert_id() {
    StateMachine sm(kLegacy);
    sm.handle(held(0, AlertType::Medical));
    for (uint32_t t = 3000, i = 0; i < kDefaultTiming.max_attempts; ++i, t += kDefaultTiming.retry_backoff_ms) {
        sm.handle(tick(t));
        sm.handle(sendFail(t, 1));
    }
    TEST_ASSERT_EQUAL_STRING("ERROR", toString(sm.state()));
    const Result late = sm.handle(ack(9000, 1));
    assertRejected(late, sm, State::Error);
    TEST_ASSERT_EQUAL_STRING("late_ack_after_error", late.reason);
    TEST_ASSERT_EQUAL_UINT16(1, sm.alertId());
    TEST_ASSERT_EQUAL_STRING("MEDICAL", toString(sm.alertType()));
    const Result wrong = sm.handle(ack(9001, 7));
    assertRejected(wrong, sm, State::Error);
    TEST_ASSERT_EQUAL_STRING("unexpected_ack", wrong.reason);
}

// Ack timeout and retry backoff deadlines straddling the millis() wrap; every
// attempt reuses the original alert id.
void test_ack_timeout_and_backoff_survive_millis_wraparound_with_same_id() {
    StateMachine sm(kLegacy);
    const uint32_t start = 0xFFFFFFFFu - 4000;
    sm.handle(held(start));
    const uint16_t id = sm.alertId();
    sm.handle(tick(start + 3000));
    sm.handle(sendOk(start + 3000, id));  // ack deadline wraps past 0
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(start + 7999)).outcome);
    assertTransition(sm.handle(tick(start + 8000)), State::AlertSent, State::Retry, Action::None);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(start + 9999)).outcome);
    assertTransition(sm.handle(tick(start + 10000)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(2, sm.attempts());
    TEST_ASSERT_EQUAL_UINT16(id, sm.alertId());
    sm.handle(sendOk(start + 10000, id));
    assertTransition(sm.handle(ack(start + 10100, id)), State::AlertSent, State::Acknowledged, Action::None);
}

// --- resend on LinkUp (T14) -----------------------------------------------

// Host back while waiting out the backoff: resend now, same id, one attempt used.
void test_link_up_during_retry_resends_immediately() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // attempt 1 sent at 3000
    assertTransition(sm.handle(linkDown(3500)), State::AlertSent, State::Retry, Action::None);  // resend due 5500
    assertTransition(sm.handle(linkUp(3800)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(2, sm.attempts());
    TEST_ASSERT_EQUAL_UINT16(1, sm.alertId());
    // The old backoff deadline must not fire a second send.
    sm.handle(sendOk(3800, 1));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(5500)).outcome);
    TEST_ASSERT_EQUAL_STRING("ALERT_SENT", toString(sm.state()));
    assertTransition(sm.handle(ack(3900, 1)), State::AlertSent, State::Acknowledged, Action::None);
}

// The attempt budget still bounds the alert: a failed last resend ends in ERROR.
void test_link_up_resend_respects_attempt_budget() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    sm.handle(sendFail(3000, 1));  // attempt 1
    sm.handle(tick(5000));
    sm.handle(sendFail(5000, 1));  // attempt 2
    assertTransition(sm.handle(linkUp(5200)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(3, sm.attempts());
    assertTransition(sm.handle(sendFail(5200, 1)), State::Confirmed, State::Error, Action::None);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(linkUp(6000)).outcome);  // ERROR stays sticky
}

// LinkUp means nothing outside RETRY.
void test_link_up_outside_retry_is_ignored() {
    StateMachine sm(kLegacy);
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(linkUp(0)).outcome);  // IDLE
    sm.handle(held(100));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(linkUp(200)).outcome);  // PENDING: no early send
    TEST_ASSERT_EQUAL_STRING("PENDING", toString(sm.state()));
    sm.handle(tick(3100));
    sm.handle(sendOk(3100, 1));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(linkUp(3200)).outcome);  // ALERT_SENT: no duplicate
    TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());
}

// --- host not ready / delivery budget (T5b, T7b) ----------------------------

// Host not connected+subscribed: nothing sent, no attempt used, probe again
// after the backoff; the first real send then carries attempt 1.
void test_not_ready_send_refunds_attempt() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    assertTransition(sm.handle(tick(3000)), State::Pending, State::Confirmed, Action::SendAlert);
    const Result r = sm.handle(notReady(3000, 1));
    assertTransition(r, State::Confirmed, State::Retry, Action::None);
    TEST_ASSERT_EQUAL_STRING("link_not_ready", r.reason);
    TEST_ASSERT_EQUAL_UINT8(0, sm.attempts());
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(4999)).outcome);
    assertTransition(sm.handle(tick(5000)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());
    assertTransition(sm.handle(sendOk(5000, 1)), State::Confirmed, State::AlertSent, Action::None);
}

// Subscription completes between probes: send at once, still attempt 1.
void test_link_up_after_not_ready_sends_first_attempt_now() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    sm.handle(notReady(3000, 1));
    assertTransition(sm.handle(linkUp(3700)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());
}

// The bench failure this fixes: the host reconnects slowly and is still not
// subscribed at every backoff probe. Not-ready probes must not use up the
// budget, so the alert is still delivered once the host is back.
void test_slow_reconnect_does_not_exhaust_attempts() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // attempt 1 sent at 3000
    sm.handle(linkDown(3100));  // RETRY, probe due 5100
    for (uint32_t t = 5100; t <= 11100; t += 2000) {
        assertTransition(sm.handle(tick(t)), State::Retry, State::Confirmed, Action::SendAlert);
        sm.handle(notReady(t, 1));
        TEST_ASSERT_EQUAL_UINT8(1, sm.attempts());
    }
    assertTransition(sm.handle(linkUp(12000)), State::Retry, State::Confirmed, Action::SendAlert);
    TEST_ASSERT_EQUAL_UINT8(2, sm.attempts());
    sm.handle(sendOk(12000, 1));
    assertTransition(sm.handle(ack(12100, 1)), State::AlertSent, State::Acknowledged, Action::None);
}

// No host at all: probes never send, and the 20 s budget from the first
// CONFIRMED ends the alert in ERROR with nothing transmitted.
void test_no_host_ends_in_error_at_delivery_timeout() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));  // first CONFIRMED: budget ends at 23000
    sm.handle(notReady(3000, 1));
    for (uint32_t t = 5000; t < 23000; t += 2000) {
        sm.handle(tick(t));
        sm.handle(notReady(t, 1));
    }
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(22999)).outcome);
    const Result r = sm.handle(tick(23000));
    assertTransition(r, State::Retry, State::Error, Action::None);
    TEST_ASSERT_EQUAL_STRING("delivery_timeout", r.reason);
    TEST_ASSERT_EQUAL_UINT8(0, sm.attempts());
}

// The budget also cuts an ack wait short: a send made late in the budget does
// not get its full ack timeout.
void test_delivery_timeout_cuts_ack_wait() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    sm.handle(notReady(3000, 1));
    sm.handle(linkUp(21000));
    sm.handle(sendOk(21000, 1));  // ack would time out at 26000
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(22999)).outcome);
    const Result r = sm.handle(tick(23000));
    assertTransition(r, State::AlertSent, State::Error, Action::None);
    TEST_ASSERT_EQUAL_STRING("delivery_timeout", r.reason);
    // A late ack is still only logged, as for any ERROR.
    TEST_ASSERT_EQUAL_STRING("late_ack_after_error", sm.handle(ack(23500, 1)).reason);
}

// Three real attempts with no ack (5 + 2 + 5 + 2 + 5 = 19 s) fit inside the
// 20 s budget, so the attempt limit, not the clock, ends this case.
void test_three_unacked_attempts_end_by_attempt_limit_within_budget() {
    StateMachine sm(kLegacy);
    driveToAlertSent(sm);  // 3000
    sm.handle(tick(8000));  // ack timeout -> RETRY
    sm.handle(tick(10000));
    sm.handle(sendOk(10000, 1));
    sm.handle(tick(15000));
    sm.handle(tick(17000));
    sm.handle(sendOk(17000, 1));
    const Result r = sm.handle(tick(22000));
    assertTransition(r, State::AlertSent, State::Error, Action::None);
    TEST_ASSERT_EQUAL_STRING("ack_timeout", r.reason);
    TEST_ASSERT_EQUAL_UINT8(3, sm.attempts());
}

void test_stale_not_ready_is_rejected() {
    StateMachine sm(kLegacy);
    assertRejected(sm.handle(notReady(0, 1)), sm, State::Idle);
    driveToAlertSent(sm);
    assertRejected(sm.handle(notReady(3100, 1)), sm, State::AlertSent);
    sm.handle(linkDown(3200));
    sm.handle(tick(5200));  // CONFIRMED, attempt 2
    assertRejected(sm.handle(notReady(5200, 9)), sm, State::Confirmed);  // wrong id
}

void test_delivery_timeout_survives_millis_wraparound() {
    StateMachine sm(kLegacy);
    const uint32_t start = 0xFFFFFFFFu - 10000;
    sm.handle(held(start));
    sm.handle(tick(start + 3000));  // budget ends at start + 23000, past the wrap
    sm.handle(notReady(start + 3000, 1));
    // 1 ms before the budget ends this is just another not-ready probe.
    assertTransition(sm.handle(tick(start + 22999)), State::Retry, State::Confirmed, Action::SendAlert);
    sm.handle(notReady(start + 22999, 1));
    TEST_ASSERT_EQUAL_STRING("delivery_timeout", sm.handle(tick(start + 23000)).reason);
}

// A new alert gets a fresh budget.
void test_new_alert_resets_delivery_budget() {
    StateMachine sm(kLegacy);
    sm.handle(held(0));
    sm.handle(tick(3000));
    sm.handle(notReady(3000, 1));
    sm.handle(tick(23000));  // ERROR
    sm.handle(held(30000, AlertType::Medical));
    sm.handle(tick(33000));  // budget for alert 2 ends at 53000
    sm.handle(notReady(33000, 2));
    // 19999 ms into alert 2's own budget it is still probing (alert 1's budget
    // ran out at 23000 and must not carry over).
    assertTransition(sm.handle(tick(52999)), State::Retry, State::Confirmed, Action::SendAlert);
    sm.handle(notReady(52999, 2));
    TEST_ASSERT_EQUAL_STRING("delivery_timeout", sm.handle(tick(53000)).reason);
}

void test_pending_deadline_survives_millis_wraparound() {
    StateMachine sm(kLegacy);
    const uint32_t start = 0xFFFFFFFFu - 1000;
    sm.handle(held(start));
    TEST_ASSERT_EQUAL(Outcome::Unchanged, sm.handle(tick(start + 2999)).outcome);
    assertTransition(sm.handle(tick(start + 3000)), State::Pending, State::Confirmed, Action::SendAlert);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_hold_send_ack_idle);
    RUN_TEST(test_cancel_during_pending_sends_nothing);
    RUN_TEST(test_cancel_after_send_notifies_host);
    RUN_TEST(test_ack_timeout_retries_then_errors_after_max_attempts);
    RUN_TEST(test_send_failure_is_bounded);
    RUN_TEST(test_link_down_while_waiting_for_ack_retries);
    RUN_TEST(test_late_ack_during_retry_is_accepted);
    RUN_TEST(test_ack_with_wrong_id_is_rejected);
    RUN_TEST(test_ack_without_alert_is_rejected);
    RUN_TEST(test_stale_send_result_is_rejected);
    RUN_TEST(test_second_hold_during_alert_is_rejected);
    RUN_TEST(test_invalid_alert_type_is_rejected);
    RUN_TEST(test_error_is_sticky_until_new_hold);
    RUN_TEST(test_new_hold_during_ack_display_starts_new_alert);
    RUN_TEST(test_cancel_press_hold_does_not_start_new_alert);
    RUN_TEST(test_cancel_press_hold_after_send_does_not_start_new_alert);
    RUN_TEST(test_lockout_rejected_press_hold_does_not_start_alert_from_error);
    RUN_TEST(test_fresh_press_outside_alert_clears_consumed_mark);
    RUN_TEST(test_press_during_cancelled_display_is_consumed);
    RUN_TEST(test_simultaneous_cancel_presses_second_hold_does_not_start_alert);
    RUN_TEST(test_default_window_is_5s_from_release);
    RUN_TEST(test_window_falls_back_to_hold_when_never_released);
    RUN_TEST(test_release_after_fallback_deadline_does_not_extend);
    RUN_TEST(test_tremor_taps_after_release_neither_cancel_nor_delay);
    RUN_TEST(test_other_button_release_does_not_start_window);
    RUN_TEST(test_cancel_boundaries_for_3s_and_5s_windows);
    RUN_TEST(test_release_timed_window_survives_millis_wraparound);
    RUN_TEST(test_simultaneous_holds_first_event_wins);
    RUN_TEST(test_held_button_repress_within_lockout_does_not_cancel);
    RUN_TEST(test_held_button_repress_after_lockout_cancels);
    RUN_TEST(test_other_button_cancels_during_lockout);
    RUN_TEST(test_held_button_repress_within_lockout_does_not_cancel_in_flight);
    RUN_TEST(test_other_button_release_does_not_arm_lockout);
    RUN_TEST(test_cancel_lockout_survives_millis_wraparound);
    RUN_TEST(test_lockout_rejected_press_is_not_replayed_after_lockout);
    RUN_TEST(test_cancel_during_retry_kills_pending_resend);
    RUN_TEST(test_late_ack_after_error_is_rejected_and_keeps_alert_id);
    RUN_TEST(test_ack_timeout_and_backoff_survive_millis_wraparound_with_same_id);
    RUN_TEST(test_link_up_during_retry_resends_immediately);
    RUN_TEST(test_link_up_resend_respects_attempt_budget);
    RUN_TEST(test_link_up_outside_retry_is_ignored);
    RUN_TEST(test_not_ready_send_refunds_attempt);
    RUN_TEST(test_link_up_after_not_ready_sends_first_attempt_now);
    RUN_TEST(test_slow_reconnect_does_not_exhaust_attempts);
    RUN_TEST(test_no_host_ends_in_error_at_delivery_timeout);
    RUN_TEST(test_delivery_timeout_cuts_ack_wait);
    RUN_TEST(test_three_unacked_attempts_end_by_attempt_limit_within_budget);
    RUN_TEST(test_stale_not_ready_is_rejected);
    RUN_TEST(test_delivery_timeout_survives_millis_wraparound);
    RUN_TEST(test_new_alert_resets_delivery_budget);
    RUN_TEST(test_pending_deadline_survives_millis_wraparound);
    return UNITY_END();
}
