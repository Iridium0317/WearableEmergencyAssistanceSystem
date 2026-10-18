#pragma once

// Onboard RGB LED (active low on the Nano 33 BLE). Driven only from the app
// thread; patterns come from lib/feedback/status_pattern.

#include <mbed.h>

#include "status_pattern.h"

class StatusLed {
public:
    StatusLed();

    // Writes the pins only when the color changes; cheap to call every tick.
    void show(const LedColor& color);

private:
    mbed::DigitalOut red_;
    mbed::DigitalOut green_;
    mbed::DigitalOut blue_;
    LedColor shown_ = kLedOff;
};
