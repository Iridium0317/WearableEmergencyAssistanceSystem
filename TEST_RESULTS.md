# Test Results

## T0 — Board bring-up smoke test (2026-10-10)

| Field | Value |
|---|---|
| Setup | Nano 33 BLE Rev2 on USB (`/dev/cu.usbmodem11201`, VID:PID 2341:805A). PlatformIO 6.1.19, `nordicnrf52`, env `nano33ble`. Firmware: `src/main.cpp` smoke test. |
| Expected | Build with no warnings in project code; upload succeeds; 1 Hz `[HB]` heartbeat on serial; I2C scan of Wire1 finds BMI270 (0x68) and BMM150 (0x10); onboard LED blinks at 1 Hz. |
| Observed | Build OK, no project-code warnings (RAM 16.7%, Flash 8.4%). Upload OK. Heartbeat received at 1000 ms intervals. Wire1 scan found 5 devices: 0x10, 0x39, 0x44, 0x5C, 0x68. BMI270 OK, BMM150 OK. 0x44 (HS3003) is consistent with Rev2. LED blinking ~1 Hz confirmed visually by user. |
| Result | PASS |

## T1 — Host unit tests: state machine + button debouncer (2026-10-10)

| Field | Value |
|---|---|
| Setup | `pio test -e native` (Apple clang 15, `-std=c++17 -Wall -Wextra -Werror`, Unity 2.6.1). Pure logic in `lib/app` and `lib/button_logic`. |
| Expected | All transitions T1–T12 behave as documented in `state_machine.h`; retries bounded by `max_attempts`; invalid/stale events rejected; debounce rejects bounce/glitches; hold fires once at exactly 1500 ms; both survive `millis()` wraparound. |
| Observed | 23/23 tests passed (15 state machine, 8 debouncer). Mutation check: changing the retry bound `>=` to `>` made 3 tests fail, confirming the tests detect that defect. |
| Result | PASS |

## T2 — Hardware bench: state machine on Nano 33 BLE via serial console (2026-10-10)

| Field | Value |
|---|---|
| Setup | Nano 33 BLE Rev2 over USB, firmware with app RTOS thread + `rtos::Mail` event queue + `SimulatedLink` transport (no BLE yet, no physical buttons yet — button input injected via `hold`/`press` console commands). `python tests/bench/bench.py`; device rebooted before each scenario. Run `20261010-212521` (CSV + raw serial log in `tests/bench/results/`). |
| Expected | Normal path IDLE→PENDING→CONFIRMED→ALERT_SENT→ACKNOWLEDGED→IDLE; cancel in PENDING sends nothing; ack timeout retries and reaches ERROR after 3 attempts; link down fails sends and reaches ERROR; link drop mid-alert retries and recovers; wrong-id / no-alert acks rejected with no state change; malformed commands answered with CMD_ERR; no malformed log lines. Timeouts within +30 ms of nominal. |
| Observed | 8/8 scenarios PASS. Device-measured timeouts (1 ms resolution): pending window 3000 ms, ack timeout 5000 ms (×3), retry backoff 2000 ms (×2), ack display 3000 ms, cancel display 1000 ms, hold→ERROR with link down 7003 ms (nominal 7000). Host serial command → state transition logged: 1.8–2.3 ms; ping RTT 1.85–2.06 ms (n=20). CONFIRMED→ALERT_SENT 1 ms. |
| Result | PASS |
| Notes | Host latencies include USB CDC round trip; they are *not* button-to-event latency (requires physical buttons, pending). An earlier run showed ~100 ms host latency; root cause was the bench tool's `ser.read(256)` waiting for its 0.1 s timeout — fixed in the tool, run discarded. |
