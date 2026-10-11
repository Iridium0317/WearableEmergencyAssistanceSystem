#include <unity.h>

#include "state_machine.h"

namespace {

Event ev(EventType type, uint32_t t, AlertType alert = AlertType::None, uint16_t id = 0) {
    return Event{type, t, alert, id};
}

Event held(uint32_t t, AlertType a = AlertType::Emergency) { return ev(EventType::ButtonHeld, t, a); }
Event pressed(uint32_t t) { return ev(EventType::ButtonPressed, t); }
Event tick(uint32_t t) { return ev(EventType::Tick, t); }
Event sendOk(uint32_t t, uint16_t id) { return ev(EventType::AlertSendOk, t, AlertType::None, id); }
Event sendFail(uint32_t t, uint16_t id) { return ev(EventType::AlertSendFailed, t, AlertType::None, id); }
Event ack(uint32_t t, uint16_t id) { return ev(EventType::HostAck, t, AlertType::None, id); }

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
    StateMachine sm;
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
    StateMachine sm;
    sm.handle(held(0));
    const Result r = sm.handle(pressed(1000));
    assertTransition(r, State::Pending, State::Cancelled, Action::None);
    TEST_ASSERT_EQUAL_STRING("user_cancel", r.reason);
    assertTransition(sm.handle(tick(2000)), State::Cancelled, State::Idle, Action::None);
}

void test_cancel_after_send_notifies_host() {
    StateMachine sm;
    driveToAlertSent(sm);
    assertTransition(sm.handle(pressed(3500)), State::AlertSent, State::Cancelled, Action::SendCancel);
}

void test_ack_timeout_retries_then_errors_after_max_attempts() {
    StateMachine sm;
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
    StateMachine sm;
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
    StateMachine sm;
    driveToAlertSent(sm);
    const Result r = sm.handle(ev(EventType::LinkDown, 3500));
    assertTransition(r, State::AlertSent, State::Retry, Action::None);
    TEST_ASSERT_EQUAL_STRING("link_down", r.reason);
}

void test_late_ack_during_retry_is_accepted() {
    StateMachine sm;
    driveToAlertSent(sm);
    sm.handle(tick(8000));  // -> RETRY
    assertTransition(sm.handle(ack(8500, sm.alertId())), State::Retry, State::Acknowledged, Action::None);
}

void test_ack_with_wrong_id_is_rejected() {
    StateMachine sm;
    driveToAlertSent(sm);
    assertRejected(sm.handle(ack(3100, sm.alertId() + 1)), sm, State::AlertSent);
}

void test_ack_without_alert_is_rejected() {
    StateMachine sm;
    assertRejected(sm.handle(ack(10, 1)), sm, State::Idle);
}

void test_stale_send_result_is_rejected() {
    StateMachine sm;
    sm.handle(held(0));
    sm.handle(tick(3000));
    assertRejected(sm.handle(sendOk(3001, sm.alertId() + 7)), sm, State::Confirmed);
}

void test_second_hold_during_alert_is_rejected() {
    StateMachine sm;
    sm.handle(held(0));
    assertRejected(sm.handle(held(100, AlertType::Family)), sm, State::Pending);
    TEST_ASSERT_EQUAL_STRING("EMERGENCY", toString(sm.alertType()));
}

void test_invalid_alert_type_is_rejected() {
    StateMachine sm;
    assertRejected(sm.handle(held(0, AlertType::None)), sm, State::Idle);
}

void test_error_is_sticky_until_new_hold() {
    StateMachine sm;
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
    StateMachine sm;
    driveToAlertSent(sm);
    sm.handle(ack(3100, sm.alertId()));
    assertTransition(sm.handle(held(3500, AlertType::Family)), State::Acknowledged, State::Pending, Action::None);
    TEST_ASSERT_EQUAL_UINT16(2, sm.alertId());
}

void test_pending_deadline_survives_millis_wraparound() {
    StateMachine sm;
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
    RUN_TEST(test_pending_deadline_survives_millis_wraparound);
    return UNITY_END();
}
