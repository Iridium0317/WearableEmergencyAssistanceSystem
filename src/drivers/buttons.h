#pragma once

// Three physical emergency buttons on D2/D3/D4, wired pin -> switch -> GND
// with the internal pull-up (pressed reads LOW).
//
// Context split:
//   GPIO ISR     : timestamp + level -> EdgeRing, wake the button task. Nothing else.
//   button task  : drain ring -> ButtonDebouncer -> post ButtonPressed/ButtonHeld/ButtonReleased
//                  to the app queue, and log button diagnostics.
//
// The task waits on a thread flag: indefinitely while all buttons are idle,
// every kServicePeriodMs while a debounce window or hold timer is running.

#include <mbed.h>

#include <atomic>

#include "button_debouncer.h"
#include "edge_ring.h"
#include "event_queue.h"

class ButtonInput {
public:
    static constexpr uint32_t kDebounceMs = 30;
    static constexpr uint32_t kHoldMs = 1500;
    // Tremor tolerance: lifts shorter than this do not restart a hold.
    // Prototype default, not validated with users.
    static constexpr uint32_t kHoldGapMs = 150;

    explicit ButtonInput(AppEventQueue& events);

    void start();

    // Any thread: logs raw pin levels and ISR edge count. For wiring checks;
    // touches only the pins and an atomic, never the task-owned debouncers.
    void reportDiagnostics();

private:
    static constexpr size_t kButtonCount = 3;
    static constexpr uint32_t kServicePeriodMs = 5;
    static constexpr uint32_t kEdgeFlag = 0x1;

    // ISR context.
    void onEdge(uint8_t button);
    void onEdge0() { onEdge(0); }
    void onEdge1() { onEdge(1); }
    void onEdge2() { onEdge(2); }

    // Button task context.
    void run();
    void drainEdges();
    void resyncAfterOverflow(uint32_t now_ms);
    void serviceButton(uint8_t button, uint32_t now_ms);
    bool anyNeedsService() const;

    AppEventQueue& events_;
    mbed::InterruptIn pin0_;
    mbed::InterruptIn pin1_;
    mbed::InterruptIn pin2_;
    mbed::InterruptIn* pins_[kButtonCount];
    ButtonDebouncer debouncers_[kButtonCount];
    EdgeRing ring_;
    std::atomic<uint32_t> overflows_{0};  // written in ISR, read by task
    std::atomic<uint32_t> isr_edges_{0};  // written in ISR, read by any thread
    bool started_ = false;
    uint32_t overflows_seen_ = 0;
    // Worst ISR-timestamp -> task-dequeue delay since the last report.
    uint32_t max_isr_to_task_us_[kButtonCount] = {};
    rtos::Thread thread_;
};
