#include "status_led.h"

#include <Arduino.h>

StatusLed::StatusLed()
    : red_(digitalPinToPinName(LEDR), 1), green_(digitalPinToPinName(LEDG), 1), blue_(digitalPinToPinName(LEDB), 1) {}

void StatusLed::show(const LedColor& color) {
    if (color == shown_) {
        return;
    }
    // Active low: 0 lights the LED.
    red_.write(color.red ? 0 : 1);
    green_.write(color.green ? 0 : 1);
    blue_.write(color.blue ? 0 : 1);
    shown_ = color;
}
