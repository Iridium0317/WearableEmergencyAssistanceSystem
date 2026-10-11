#include "event_queue.h"

bool AppEventQueue::post(const Event& event) {
    Event* slot = mail_.try_alloc();
    if (slot == nullptr) {
        dropped_.fetch_add(1);
        return false;
    }
    *slot = event;
    mail_.put(slot);  // cannot fail: the queue has a slot for every pool block
    return true;
}

bool AppEventQueue::wait(Event& out, uint32_t timeout_ms) {
    Event* slot = mail_.try_get_for(std::chrono::milliseconds(timeout_ms));
    if (slot == nullptr) {
        return false;
    }
    out = *slot;
    mail_.free(slot);
    return true;
}
