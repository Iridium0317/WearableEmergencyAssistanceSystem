// Bring-up smoke test: proves toolchain, flashing, USB serial, and the
// internal IMU I2C bus (Wire1) all work before real firmware is written.
#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint32_t kBlinkPeriodMs = 500;
constexpr uint32_t kReportPeriodMs = 1000;
constexpr uint8_t kBmi270Addr = 0x68;
constexpr uint8_t kBmm150Addr = 0x10;

bool i2cDevicePresent(uint8_t addr) {
    Wire1.beginTransmission(addr);
    return Wire1.endTransmission() == 0;
}

void scanImuBus() {
    Serial.println("[I2C] scanning Wire1 (internal sensor bus)");
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2cDevicePresent(addr)) {
            Serial.print("[I2C]   device at 0x");
            Serial.println(addr, HEX);
            ++found;
        }
    }
    Serial.print("[I2C] total devices: ");
    Serial.println(found);
    Serial.print("[I2C] BMI270 (0x68): ");
    Serial.println(i2cDevicePresent(kBmi270Addr) ? "OK" : "MISSING");
    Serial.print("[I2C] BMM150 (0x10): ");
    Serial.println(i2cDevicePresent(kBmm150Addr) ? "OK" : "MISSING");
}

}  // namespace

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);

    // USB CDC: wait briefly for the host so the boot report isn't lost,
    // but never block forever when running untethered.
    const uint32_t start = millis();
    while (!Serial && (millis() - start) < 3000) {
    }

    Serial.println("[BOOT] Nano 33 BLE smoke test");
    Wire1.begin();
    scanImuBus();
}

void loop() {
    static uint32_t lastBlink = 0;
    static uint32_t lastReport = 0;
    static bool ledOn = false;
    const uint32_t now = millis();

    if (now - lastBlink >= kBlinkPeriodMs) {
        lastBlink = now;
        ledOn = !ledOn;
        digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
    }

    if (now - lastReport >= kReportPeriodMs) {
        lastReport = now;
        Serial.print("[HB] uptime_ms=");
        Serial.println(now);
    }

    // 's' lets the host re-request the scan, since the boot report is
    // usually printed before the host opens the port.
    if (Serial.available() > 0 && Serial.read() == 's') {
        scanImuBus();
    }
}
