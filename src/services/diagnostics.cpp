#include "diagnostics.h"

#include <Arduino.h>
#include <inttypes.h>
#include <mbed.h>
#include <stdarg.h>
#include <stdio.h>

namespace diag {
namespace {

constexpr size_t kLineSize = 160;

// Serial is shared by the app thread and the console thread; the mutex also
// guards line_ so the buffer is static rather than on each caller's stack.
rtos::Mutex g_mutex;
char g_line[kLineSize];

}  // namespace

void log(const char* tag, const char* fmt, ...) {
    g_mutex.lock();
    int len = snprintf(g_line, kLineSize, "%" PRIu32 " %s ", static_cast<uint32_t>(millis()), tag);
    if (len > 0 && static_cast<size_t>(len) < kLineSize) {
        va_list args;
        va_start(args, fmt);
        const int body = vsnprintf(g_line + len, kLineSize - len, fmt, args);
        va_end(args);
        if (body > 0) {
            len += body;
        }
    }
    Serial.println(g_line);
    if (static_cast<size_t>(len) >= kLineSize) {
        // Never truncate silently: the bench tool treats this as a defect.
        Serial.println("0 DIAG_ERR reason=line_truncated");
    }
    g_mutex.unlock();
}

}  // namespace diag
