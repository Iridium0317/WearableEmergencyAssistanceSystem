// Wiring only: construct modules, start the app thread, run the console.
#include <Arduino.h>
#include <inttypes.h>

#include "buttons.h"
#include "command_console.h"
#include "diagnostics.h"
#include "emergency_controller.h"
#include "event_queue.h"
#include "status_led.h"

#ifdef USE_SIMULATED_LINK
#include "simulated_link.h"
#else
#include "ble_link.h"
#endif

namespace {

constexpr uint32_t kHeartbeatPeriodMs = 5000;
constexpr uint32_t kSerialWaitMs = 3000;

AppEventQueue g_events;

#ifdef USE_SIMULATED_LINK
SimulatedLink g_link;
ButtonInput g_buttons(g_events);
CommandConsole g_console(g_events, &g_link, g_buttons);
#else
BleLink g_link(g_events);
ButtonInput g_buttons(g_events);
CommandConsole g_console(g_events, nullptr, g_buttons);
#endif

StatusLed g_led;
EmergencyController g_controller(g_events, g_link, g_led);

}  // namespace

void setup() {
    Serial.begin(115200);
    // Give a host time to open the port so the boot line isn't lost, but
    // never block forever: the device must also run untethered.
    const uint32_t start = millis();
    while (!Serial && (millis() - start) < kSerialWaitMs) {
    }
    diag::log("BOOT", "fw=wearable-emergency transport=%s", g_link.name());
    g_controller.start();
    g_buttons.start();
}

void loop() {
    static uint32_t last_heartbeat = 0;
    g_console.poll();

    const uint32_t now = millis();
    if (now - last_heartbeat >= kHeartbeatPeriodMs) {
        last_heartbeat = now;
        diag::log("HB", "q_dropped=%" PRIu32, g_events.dropped());
    }
    delay(2);  // yields this thread; console latency is not critical
}
