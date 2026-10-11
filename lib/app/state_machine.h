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
//  T6  CONFIRMED|ALERT_SENT|RETRY   HostAck(id)        ACKNOWLEDGED  late acks still count: the host has it
//  T7  ALERT_SENT                   Tick >= deadline   RETRY|ERROR   ack timeout
//  T8  ALERT_SENT                   LinkDown           RETRY|ERROR   ack can no longer arrive
//  T9  RETRY                        Tick >= deadline   CONFIRMED     SendAlert (attempt n+1) after backoff
//  T10 CONFIRMED|ALERT_SENT|RETRY   ButtonPressed      CANCELLED     SendCancel (host may hold the alert)
//  T11 ACKNOWLEDGED|CANCELLED       Tick >= deadline   IDLE          status display period over
//  T12 ACKNOWLEDGED|CANCELLED|ERROR ButtonHeld(type)   PENDING       never drop a new alert
//
// ERROR is deliberately sticky (only T12 leaves it): an undelivered emergency
// must stay visible to the user rather than silently reverting to IDLE.
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
    Unchanged,     // event irrelevant in this state; not worth logging
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
};

constexpr Timing kDefaultTiming = {
    3000,  // pending_ms
    5000,  // ack_timeout_ms
    2000,  // retry_backoff_ms
    3,     // max_attempts
    3000,  // ack_display_ms
    1000,  // cancel_display_ms
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
    Result acknowledge(const Event& event);
    Result cancelInFlight(const Event& event);

    Result transition(State to, Action action, const char* reason = nullptr);
    Result unchanged() const;
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
};

const char* toString(State state);
const char* toString(Action action);
