#include "command_console.h"

#include <Arduino.h>
#include <inttypes.h>
#include <mbed.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics.h"

namespace {

// Parses a whole token as a base-10 integer; rejects "", "12x", etc.
bool parseInt(const char* token, long& out) {
    if (token == nullptr || *token == '\0') {
        return false;
    }
    char* end = nullptr;
    out = strtol(token, &end, 10);
    return *end == '\0';
}

}  // namespace

CommandConsole::CommandConsole(AppEventQueue& events, SimulatedLink* link, ButtonInput& buttons)
    : events_(events), link_(link), buttons_(buttons) {}

void CommandConsole::poll() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (length_ < kLineSize - 1) {
                line_[length_++] = c;
            } else {
                overflow_ = true;
            }
            continue;
        }
        line_[length_] = '\0';
        if (overflow_) {
            diag::log("CMD_ERR", "reason=line_too_long max=%u", static_cast<unsigned>(kLineSize - 1));
        } else if (length_ > 0) {
            execute(line_);
        }
        length_ = 0;
        overflow_ = false;
    }
}

void CommandConsole::execute(char* line) {
    const char* cmd = strtok(line, " ");
    const char* arg = strtok(nullptr, " ");
    const char* extra = strtok(nullptr, " ");
    long value = 0;

    if (extra != nullptr) {
        diag::log("CMD_ERR", "cmd=%s reason=too_many_args", cmd);
    } else if (strcmp(cmd, "ping") == 0 && arg == nullptr) {
        diag::log("CMD_OK", "cmd=ping");
    } else if (strcmp(cmd, "btn") == 0 && arg == nullptr) {
        buttons_.reportDiagnostics();
        diag::log("CMD_OK", "cmd=btn");
    } else if (strcmp(cmd, "reboot") == 0 && arg == nullptr) {
        diag::log("CMD_OK", "cmd=reboot");
        delay(100);  // let the reply drain over USB before the reset
        NVIC_SystemReset();
    } else if (strcmp(cmd, "hold") == 0) {
        const AlertType type = parseInt(arg, value) ? alertTypeFromIndex(value) : AlertType::None;
        if (type == AlertType::None) {
            diag::log("CMD_ERR", "cmd=hold reason=bad_button expected=1..3");
        } else if (post(EventType::ButtonPressed, type) && post(EventType::ButtonHeld, type)) {
            diag::log("CMD_OK", "cmd=hold alert=%s", toString(type));
        }
    } else if (strcmp(cmd, "press") == 0 && arg == nullptr) {
        if (post(EventType::ButtonPressed)) {
            diag::log("CMD_OK", "cmd=press");
        }
    } else if (strcmp(cmd, "ack") == 0) {
        if (!parseInt(arg, value) || value < 1 || value > 0xFFFF) {
            diag::log("CMD_ERR", "cmd=ack reason=bad_id expected=1..65535");
        } else if (post(EventType::HostAck, AlertType::None, static_cast<uint16_t>(value))) {
            diag::log("CMD_OK", "cmd=ack id=%ld", value);
        }
    } else if (strcmp(cmd, "link") == 0 && link_ == nullptr) {
        diag::log("CMD_ERR", "cmd=link reason=not_simulated");
    } else if (strcmp(cmd, "link") == 0 && arg != nullptr &&
               (strcmp(arg, "up") == 0 || strcmp(arg, "down") == 0)) {
        const bool up = strcmp(arg, "up") == 0;
        link_->setUp(up);
        if (post(up ? EventType::LinkUp : EventType::LinkDown)) {
            diag::log("CMD_OK", "cmd=link up=%d", up ? 1 : 0);
        }
    } else {
        diag::log("CMD_ERR", "cmd=%s reason=unknown_or_bad_args", cmd);
    }
}

bool CommandConsole::post(EventType type, AlertType alert, uint16_t id) {
    if (!events_.post(Event{type, millis(), alert, id})) {
        diag::log("CMD_ERR", "reason=queue_full dropped=%" PRIu32, events_.dropped());
        return false;
    }
    return true;
}
