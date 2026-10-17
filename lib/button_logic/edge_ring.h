#pragma once

// Lock-free single-producer / single-consumer ring of raw button edges.
//
// Producer: the GPIO interrupt context. All three button pins share the GPIOTE
// interrupt, so their handlers never preempt each other and there really is a
// single producer. Consumer: the button task. No locks or critical sections:
// each index is written by exactly one side, and acquire/release ordering
// makes a slot's contents visible before the index that publishes it.
//
// One slot is kept empty to tell "full" from "empty", so it holds kCapacity-1.

#include <stddef.h>
#include <stdint.h>

#include <atomic>

struct ButtonEdge {
    uint8_t button;     // 0..2
    bool pressed;       // level after the edge (true = contact closed)
    uint32_t t_ms;      // millis() in the ISR, for the debouncer
    uint32_t t_us;      // micros() in the ISR, for latency measurement
};

class EdgeRing {
public:
    static constexpr size_t kCapacity = 32;

    // Producer side only. Returns false when full (edge dropped).
    bool push(const ButtonEdge& edge) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % kCapacity;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head] = edge;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only. Returns false when empty.
    bool pop(ButtonEdge& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[tail];
        tail_.store((tail + 1) % kCapacity, std::memory_order_release);
        return true;
    }

private:
    ButtonEdge slots_[kCapacity] = {};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
