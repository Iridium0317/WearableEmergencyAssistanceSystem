#pragma once

// Emergency alert state machine. Pure logic: no I/O, no clock reads, no
// platform headers. Time arrives via Event::timestamp_ms, and side effects are
// returned as an Action for the caller (EmergencyController) to execute.
//
// Transition table (attempts are bounded by Timing::max_attempts):
//
//  #   Source                       Event              Dest          Action / notes
//  T1  IDLE                         ButtonHeld(type)   PENDING       new alert_id; start pending window
//  T2  PENDING                      ButtonPressed      CANCELLED     nothing sent yet, so no host traffic
//  T3  PENDING                      Tick >= deadline   CONFIRMED     SendAlert (attempt 1)
//  T4  CONFIRMED                    AlertSendOk(id)    ALERT_SENT    start ack timeout
//  T5  CONFIRMED                    AlertSendFailed    RETRY|ERROR   ERROR once attempts exhausted
//  T5b CONFIRMED                    AlertSendNotReady  RETRY         nothing sent: attempt refunded, wait for
//                                                                    LinkUp (T14) or the next backoff probe
//  T6  CONFIRMED|ALERT_SENT|RETRY   HostAck(id)        ACKNOWLEDGED  late acks still count: the host has it
//  T7  ALERT_SENT                   Tick >= deadline   RETRY|ERROR   ack timeout
//  T7b ALERT_SENT|RETRY             Tick >= delivery   ERROR         delivery_timeout: no ack within
//                                                                    delivery_timeout_ms of the first send
//  T8  ALERT_SENT                   LinkDown           RETRY|ERROR   ack can no longer arrive
//  T9  RETRY                        Tick >= deadline   CONFIRMED     SendAlert (attempt n+1) after backoff
//  T10 CONFIRMED|ALERT_SENT|RETRY   ButtonPressed      CANCELLED     SendCancel (host may hold the alert)
//  T11 ACKNOWLEDGED|CANCELLED       Tick >= deadline   IDLE          status display period over
//  T12 ACKNOWLEDGED|CANCELLED|ERROR ButtonHeld(type)   PENDING       never drop a new alert
//  T13 PENDING                      ButtonReleased     (PENDING)     own button's first release restarts
//                                                                    the window (no state change)
//  T14 RETRY                        LinkUp             CONFIRMED     SendAlert (attempt n+1) now: the host
//                                                                    is back, don't wait out the backoff
//
// Pending window start: with pending_from_release, the window is timed from
// the first debounced release of the alert's own button, so a user who keeps
// holding does not lose the cancel time. Until that release the deadline is
// HELD + pending_ms, so an alert still goes out if the button is never let go
// (e.g. the user is lying on it); a release after that deadline does not
// extend it.
//
// Overlapping buttons: any button's press is the cancel gesture (T2/T10).
// Cancelling and starting an alert are separate actions: a press that arrives
// while an alert is active, or during the CANCELLED display (so the second of
// two near-simultaneous cancel presses is covered), is consumed, and its own
// ButtonHeld can never start a new alert (T1/T12 reject it), even after the
// machine has left the alert.
// Only a release followed by a fresh press can. So a user who held the wrong
// button cancels it, lets go, and holds the intended one. Holds that arrive
// while an alert is active are rejected, so for holds in the same instant the
// first event wins.
//
// Cancel lockout (wobble guard): for cancel_lockout_ms after the alert's own
// button is released, a press of that same button is rejected instead of
// cancelling (T2/T10). A shaky finger slipping off and back on is a real
// contact change that debouncing cannot filter (hardware showed a single clean
// 48 ms open). Other buttons cancel at any time.
//
// Delivery budget: at most max_attempts real transmissions (sent, or tried
// and failed in the transport) and at most delivery_timeout_ms from the first
// CONFIRMED, whichever ends first. Probes that find the host not ready send
// nothing and cost no attempt, but the clock keeps running, so an alert with
// no reachable host still ends in ERROR.
//
// ERROR is deliberately sticky (only T12 leaves it): an undelivered emergency
// must stay visible to the user rather than silently reverting to IDLE. A late
// HostAck for the failed alert is rejected as late_ack_after_error (alert_id
// kept) instead of turning the failure into ACKNOWLEDGED after the fact.
//
// Events not in the table either leave the state unchanged silently (normal
// noise such as Tick with no deadline, or the ButtonPressed that precedes
// every ButtonHeld) or are Rejected with a reason so they can be logged.

#include <stdint.h>

#include "event.h"

enum class State : uint8_t {
    Idle,
    Pending,
    Confirmed,
    AlertSent,
    Acknowledged,
    Cancelled,
    Retry,
    Error,
};

enum class Action : uint8_t {
    None,
    SendAlert,
    SendCancel,
};

enum class Outcome : uint8_t {
    Unchanged,     // no transition; worth logging only if reason is set
    Transitioned,
    Rejected,      // event invalid in this state; caller should report it
};

struct Result {
    Outcome outcome;
    State from;
    State to;
    Action action;
    const char* reason;  // set for Rejected and for transitions with a non-obvious cause
};

struct Timing {
    uint32_t pending_ms;        // cancel window after a deliberate hold
    uint32_t ack_timeout_ms;    // wait for host ack after a successful send
    uint32_t retry_backoff_ms;  // pause between send attempts
    uint8_t max_attempts;       // total send attempts per alert, including the first
    uint32_t ack_display_ms;    // how long ACKNOWLEDGED is shown before IDLE
    uint32_t cancel_display_ms; // how long CANCELLED is shown before IDLE
    uint32_t cancel_lockout_ms; // own-button re-press ignored this long after its release
    bool pending_from_release;  // time the pending window from the own button's release (T13)
    uint32_t delivery_timeout_ms;  // first send -> ERROR if still unacknowledged (T7b)
};

// Prototype defaults, not medically or ergonomically validated.
constexpr Timing kDefaultTiming = {
    5000,  // pending_ms
    5000,  // ack_timeout_ms
    2000,  // retry_backoff_ms
    3,     // max_attempts
    3000,  // ack_display_ms
    1000,  // cancel_display_ms
    300,   // cancel_lockout_ms
    true,  // pending_from_release
    20000, // delivery_timeout_ms (covers ~5 s observed BLE reconnects with margin)
};

class StateMachine {
public:
    explicit StateMachine(const Timing& timing = kDefaultTiming);

    Result handle(const Event& event);

    State state() const { return state_; }
    AlertType alertType() const { return alert_type_; }
    uint16_t alertId() const { return alert_id_; }
    uint8_t attempts() const { return attempts_; }

private:
    Result onIdle(const Event& event);
    Result onPending(const Event& event);
    Result onConfirmed(const Event& event);
    Result onAlertSent(const Event& event);
    Result onRetry(const Event& event);
    Result onDisplay(const Event& event);  // ACKNOWLEDGED and CANCELLED
    Result onError(const Event& event);

    Result startAlert(const Event& event);
    Result sendAttempt(const Event& event);
    Result failAttempt(const Event& event, const char* reason);
    Result deferAttempt(const Event& event);
    bool deliveryExpired(uint32_t now) const;
    Result acknowledge(const Event& event);
    Result cancelInFlight(const Event& event);
    Result noteRelease(const Event& event);
    Result onPendingRelease(const Event& event);
    bool inCancelLockout(const Event& event) const;
    void trackPress(const Event& event);
    bool pressConsumed(AlertType alert) const;
    bool consumesPresses() const;

    Result transition(State to, Action action, const char* reason = nullptr);
    Result unchanged() const;
    Result note(const char* reason) const;  // Unchanged, but worth logging
    Result reject(const char* reason) const;

    bool deadlineReached(uint32_t now) const;
    void setDeadline(uint32_t now, uint32_t duration_ms);

    const Timing timing_;
    State state_ = State::Idle;
    AlertType alert_type_ = AlertType::None;
    uint16_t alert_id_ = 0;
    uint8_t attempts_ = 0;
    bool deadline_armed_ = false;
    uint32_t deadline_ms_ = 0;
    bool lockout_armed_ = false;
    uint32_t lockout_until_ms_ = 0;
    uint8_t consumed_presses_ = 0;  // bit per AlertType: press arrived during an alert
    bool awaiting_release_ = false;  // pending window not yet restarted by T13
    bool delivery_armed_ = false;
    uint32_t delivery_deadline_ms_ = 0;
};

const char* toString(State state);
const char* toString(Action action);
