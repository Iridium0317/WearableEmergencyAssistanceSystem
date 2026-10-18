#include "state_machine.h"

StateMachine::StateMachine(const Timing& timing) : timing_(timing) {}

Result StateMachine::handle(const Event& event) {
    trackPress(event);
    switch (state_) {
        case State::Idle: return onIdle(event);
        case State::Pending: return onPending(event);
        case State::Confirmed: return onConfirmed(event);
        case State::AlertSent: return onAlertSent(event);
        case State::Retry: return onRetry(event);
        case State::Acknowledged:
        case State::Cancelled: return onDisplay(event);
        case State::Error: return onError(event);
    }
    return reject("unknown_state");
}

// --- per-state handlers ----------------------------------------------------

Result StateMachine::onIdle(const Event& event) {
    switch (event.type) {
        case EventType::ButtonHeld: return startAlert(event);  // T1
        case EventType::HostAck: return reject("ack_without_alert");
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

Result StateMachine::onPending(const Event& event) {
    switch (event.type) {
        case EventType::ButtonPressed:  // T2
            if (inCancelLockout(event)) {
                return reject("cancel_lockout");
            }
            setDeadline(event.timestamp_ms, timing_.cancel_display_ms);
            return transition(State::Cancelled, Action::None, "user_cancel");
        case EventType::Tick:
            if (deadlineReached(event.timestamp_ms)) {
                return sendAttempt(event);  // T3
            }
            return unchanged();
        case EventType::ButtonHeld: return reject("alert_in_progress");
        case EventType::ButtonReleased: return onPendingRelease(event);  // T13
        case EventType::HostAck: return reject("ack_before_send");
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

Result StateMachine::onConfirmed(const Event& event) {
    switch (event.type) {
        case EventType::AlertSendOk:
            if (event.alert_id != alert_id_) {
                return reject("stale_send_result");
            }
            setDeadline(event.timestamp_ms, timing_.ack_timeout_ms);
            return transition(State::AlertSent, Action::None);  // T4
        case EventType::AlertSendFailed:
            if (event.alert_id != alert_id_) {
                return reject("stale_send_result");
            }
            return failAttempt(event, "send_failed");  // T5
        case EventType::AlertSendNotReady:
            if (event.alert_id != alert_id_) {
                return reject("stale_send_result");
            }
            return deferAttempt(event);  // T5b
        case EventType::HostAck: return acknowledge(event);       // T6
        case EventType::ButtonPressed: return cancelInFlight(event);  // T10
        case EventType::ButtonHeld: return reject("alert_in_progress");
        case EventType::ButtonReleased: return noteRelease(event);
        default: return unchanged();
    }
}

Result StateMachine::onAlertSent(const Event& event) {
    switch (event.type) {
        case EventType::HostAck: return acknowledge(event);  // T6
        case EventType::Tick:
            if (deliveryExpired(event.timestamp_ms)) {
                deadline_armed_ = false;
                return transition(State::Error, Action::None, "delivery_timeout");  // T7b
            }
            if (deadlineReached(event.timestamp_ms)) {
                return failAttempt(event, "ack_timeout");  // T7
            }
            return unchanged();
        case EventType::LinkDown: return failAttempt(event, "link_down");  // T8
        case EventType::ButtonPressed: return cancelInFlight(event);       // T10
        case EventType::ButtonHeld: return reject("alert_in_progress");
        case EventType::ButtonReleased: return noteRelease(event);
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

Result StateMachine::onRetry(const Event& event) {
    switch (event.type) {
        case EventType::Tick:
            if (deliveryExpired(event.timestamp_ms)) {
                deadline_armed_ = false;
                return transition(State::Error, Action::None, "delivery_timeout");  // T7b
            }
            if (deadlineReached(event.timestamp_ms)) {
                return sendAttempt(event);  // T9
            }
            return unchanged();
        case EventType::HostAck: return acknowledge(event);            // T6
        case EventType::LinkUp: return sendAttempt(event);            // T14
        case EventType::ButtonPressed: return cancelInFlight(event);  // T10
        case EventType::ButtonHeld: return reject("alert_in_progress");
        case EventType::ButtonReleased: return noteRelease(event);
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

Result StateMachine::onDisplay(const Event& event) {
    switch (event.type) {
        case EventType::Tick:
            if (deadlineReached(event.timestamp_ms)) {
                deadline_armed_ = false;
                return transition(State::Idle, Action::None);  // T11
            }
            return unchanged();
        case EventType::ButtonHeld: return startAlert(event);  // T12
        case EventType::HostAck:
            // A duplicate ack for the alert we just completed is harmless.
            if (state_ == State::Acknowledged && event.alert_id == alert_id_) {
                return unchanged();
            }
            return reject("unexpected_ack");
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

Result StateMachine::onError(const Event& event) {
    switch (event.type) {
        case EventType::ButtonHeld: return startAlert(event);  // T12
        case EventType::HostAck:
            // The host did get this alert, but too late: stay in ERROR (the
            // user was already told it failed) and let the caller log it.
            if (event.alert_id == alert_id_) {
                return reject("late_ack_after_error");
            }
            return reject("unexpected_ack");
        case EventType::AlertSendOk:
        case EventType::AlertSendFailed:
        case EventType::AlertSendNotReady: return reject("stale_send_result");
        default: return unchanged();
    }
}

// --- shared transition helpers ---------------------------------------------

Result StateMachine::startAlert(const Event& event) {
    if (event.alert == AlertType::None) {
        return reject("invalid_alert_type");
    }
    if (pressConsumed(event.alert)) {
        return reject("hold_after_consumed_press");
    }
    alert_type_ = event.alert;
    ++alert_id_;
    if (alert_id_ == 0) {
        alert_id_ = 1;  // 0 is reserved as "no alert" on the wire
    }
    attempts_ = 0;
    delivery_armed_ = false;
    lockout_armed_ = false;
    awaiting_release_ = timing_.pending_from_release;
    setDeadline(event.timestamp_ms, timing_.pending_ms);  // fallback if never released
    return transition(State::Pending, Action::None);
}

Result StateMachine::sendAttempt(const Event& event) {
    ++attempts_;
    if (!delivery_armed_) {
        delivery_deadline_ms_ = event.timestamp_ms + timing_.delivery_timeout_ms;
        delivery_armed_ = true;
    }
    deadline_armed_ = false;  // CONFIRMED waits on the send result, not time
    return transition(State::Confirmed, Action::SendAlert);
}

Result StateMachine::failAttempt(const Event& event, const char* reason) {
    if (attempts_ >= timing_.max_attempts) {
        deadline_armed_ = false;
        return transition(State::Error, Action::None, reason);
    }
    setDeadline(event.timestamp_ms, timing_.retry_backoff_ms);
    return transition(State::Retry, Action::None, reason);
}

Result StateMachine::deferAttempt(const Event& event) {
    --attempts_;  // nothing went out, so this probe does not use the budget
    setDeadline(event.timestamp_ms, timing_.retry_backoff_ms);
    return transition(State::Retry, Action::None, "link_not_ready");
}

bool StateMachine::deliveryExpired(uint32_t now) const {
    return delivery_armed_ && static_cast<int32_t>(now - delivery_deadline_ms_) >= 0;
}

Result StateMachine::acknowledge(const Event& event) {
    if (event.alert_id != alert_id_) {
        return reject("ack_id_mismatch");
    }
    setDeadline(event.timestamp_ms, timing_.ack_display_ms);
    return transition(State::Acknowledged, Action::None);
}

Result StateMachine::cancelInFlight(const Event& event) {
    if (inCancelLockout(event)) {
        return reject("cancel_lockout");
    }
    setDeadline(event.timestamp_ms, timing_.cancel_display_ms);
    return transition(State::Cancelled, Action::SendCancel, "user_cancel");
}

Result StateMachine::onPendingRelease(const Event& event) {
    noteRelease(event);
    if (!awaiting_release_ || event.alert != alert_type_) {
        return unchanged();
    }
    awaiting_release_ = false;
    if (deadlineReached(event.timestamp_ms)) {
        return unchanged();  // window already ran out while held; the next Tick sends
    }
    setDeadline(event.timestamp_ms, timing_.pending_ms);
    return note("cancel_window_started");
}

Result StateMachine::noteRelease(const Event& event) {
    if (event.alert == alert_type_) {
        lockout_until_ms_ = event.timestamp_ms + timing_.cancel_lockout_ms;
        lockout_armed_ = true;
    }
    return unchanged();
}

void StateMachine::trackPress(const Event& event) {
    if (event.alert == AlertType::None) {
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(event.alert));
    if (event.type == EventType::ButtonPressed) {
        // Decided by the state the press arrives in. A press outside an alert
        // also clears a stale mark, so a lost release event cannot lock a
        // button out of starting alerts.
        if (consumesPresses()) {
            consumed_presses_ |= bit;
        } else {
            consumed_presses_ &= static_cast<uint8_t>(~bit);
        }
    } else if (event.type == EventType::ButtonReleased) {
        consumed_presses_ &= static_cast<uint8_t>(~bit);
    }
}

bool StateMachine::pressConsumed(AlertType alert) const {
    return (consumed_presses_ & (1u << static_cast<uint8_t>(alert))) != 0;
}

bool StateMachine::consumesPresses() const {
    return state_ == State::Pending || state_ == State::Confirmed || state_ == State::AlertSent ||
           state_ == State::Retry || state_ == State::Cancelled;
}

bool StateMachine::inCancelLockout(const Event& event) const {
    // Same signed-difference wrap handling as deadlineReached().
    return lockout_armed_ && event.alert == alert_type_ &&
           static_cast<int32_t>(event.timestamp_ms - lockout_until_ms_) < 0;
}

Result StateMachine::transition(State to, Action action, const char* reason) {
    const State from = state_;
    state_ = to;
    return Result{Outcome::Transitioned, from, to, action, reason};
}

Result StateMachine::unchanged() const {
    return Result{Outcome::Unchanged, state_, state_, Action::None, nullptr};
}

Result StateMachine::note(const char* reason) const {
    return Result{Outcome::Unchanged, state_, state_, Action::None, reason};
}

Result StateMachine::reject(const char* reason) const {
    return Result{Outcome::Rejected, state_, state_, Action::None, reason};
}

bool StateMachine::deadlineReached(uint32_t now) const {
    // Signed difference keeps this correct across the 49.7-day millis() wrap.
    return deadline_armed_ && static_cast<int32_t>(now - deadline_ms_) >= 0;
}

void StateMachine::setDeadline(uint32_t now, uint32_t duration_ms) {
    deadline_ms_ = now + duration_ms;
    deadline_armed_ = true;
}

// --- names for logs and tests ----------------------------------------------

const char* toString(State state) {
    switch (state) {
        case State::Idle: return "IDLE";
        case State::Pending: return "PENDING";
        case State::Confirmed: return "CONFIRMED";
        case State::AlertSent: return "ALERT_SENT";
        case State::Acknowledged: return "ACKNOWLEDGED";
        case State::Cancelled: return "CANCELLED";
        case State::Retry: return "RETRY";
        case State::Error: return "ERROR";
    }
    return "UNKNOWN";
}

const char* toString(Action action) {
    switch (action) {
        case Action::None: return "NONE";
        case Action::SendAlert: return "SEND_ALERT";
        case Action::SendCancel: return "SEND_CANCEL";
    }
    return "UNKNOWN";
}
