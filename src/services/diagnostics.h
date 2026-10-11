#pragma once

// Line-oriented diagnostic log over USB serial.
//
// Every line is "<uptime_ms> <TAG> key=value ..." so the Python bench tool can
// parse it without guessing. Lines are formatted into a fixed buffer and
// written under a mutex so lines from different threads never interleave.
//
// Thread context only: the mutex makes this unusable from an ISR.

namespace diag {

void log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace diag
