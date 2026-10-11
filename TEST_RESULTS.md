# Test Results

## T0 — Board bring-up smoke test (2026-10-10)

| Field | Value |
|---|---|
| Setup | Nano 33 BLE Rev2 on USB (`/dev/cu.usbmodem11201`, VID:PID 2341:805A). PlatformIO 6.1.19, `nordicnrf52`, env `nano33ble`. Firmware: `src/main.cpp` smoke test. |
| Expected | Build with no warnings in project code; upload succeeds; 1 Hz `[HB]` heartbeat on serial; I2C scan of Wire1 finds BMI270 (0x68) and BMM150 (0x10); onboard LED blinks at 1 Hz. |
| Observed | Build OK, no project-code warnings (RAM 16.7%, Flash 8.4%). Upload OK. Heartbeat received at 1000 ms intervals. Wire1 scan found 5 devices: 0x10, 0x39, 0x44, 0x5C, 0x68. BMI270 OK, BMM150 OK. 0x44 (HS3003) is consistent with Rev2. LED blinking ~1 Hz confirmed visually by user. |
| Result | PASS |
