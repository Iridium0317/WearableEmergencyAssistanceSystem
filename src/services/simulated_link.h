#pragma once

// Stand-in host link driven from the serial console ("link up|down", "ack N"),
// so the full alert flow can be exercised on hardware before BLE exists.

#include <atomic>

#include "alert_transport.h"

class SimulatedLink : public AlertTransport {
public:
    SendResult sendAlert(uint16_t alert_id, AlertType type, uint8_t attempt) override;
    bool sendCancel(uint16_t alert_id, AlertType type) override;
    const char* name() const override { return "SIM"; }

    // Console thread writes, app thread reads: atomic, no lock needed.
    void setUp(bool up) { up_.store(up); }
    bool isUp() const { return up_.load(); }

private:
    std::atomic<bool> up_{true};
};
