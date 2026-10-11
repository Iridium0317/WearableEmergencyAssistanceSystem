#pragma once

// Multi-producer / single-consumer queue feeding the EmergencyController.
//
// Producers: console thread today, button task and BLE callbacks later, and
// potentially ISRs (post() is ISR-safe). Consumer: the app thread only.
// Backed by rtos::Mail, a statically sized pool, so posting never touches the
// heap and a full queue fails fast instead of blocking the producer.

#include <mbed.h>

#include <atomic>

#include "event.h"

class AppEventQueue {
public:
    static constexpr uint32_t kCapacity = 16;

    // ISR-safe, non-blocking. Returns false (and counts a drop) when full.
    bool post(const Event& event);

    // App thread only. Returns false on timeout.
    bool wait(Event& out, uint32_t timeout_ms);

    uint32_t dropped() const { return dropped_.load(); }

private:
    rtos::Mail<Event, kCapacity> mail_;
    std::atomic<uint32_t> dropped_{0};
};
