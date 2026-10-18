# Test Results

Board: all runs below used the same board, an Arduino Nano 33 BLE **Sense** Rev2.
Entries before 2026-10-17 call it "Nano 33 BLE Rev2"; the T0 I2C scan already
showed the Sense-only sensors (0x39 APDS9960, 0x44 HS3003, 0x5C LPS22HB).

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

## T3 — Host unit tests: BLE packet protocol (2026-10-10)

| Field | Value |
|---|---|
| Setup | `pio test -e native`; `lib/protocol/alert_protocol` (alert/cancel/status encoding, ack decoding). |
| Expected | Exact little-endian byte layouts; acks rejected for wrong length, version, type, or zero id, without touching the output id. |
| Observed | 6/6 protocol tests passed; full host suite 29/29. |
| Result | PASS |

## T4 — BLE bring-up failure and fix: thread priority starvation (2026-10-10)

| Field | Value |
|---|---|
| Setup | First BLE build, app thread (which calls `BLE.begin()`/`BLE.poll()`) at `osPriorityAboveNormal`. |
| Expected | `BLE ev=advertising`. |
| Observed | `BLE_ERR op=begin reason=init_failed`; state machine kept running (`link_ok=0`), i.e. failed safely. Board later stopped responding to serial and the 1200-baud bootloader touch; recovered with a manual double-tap reset. Root cause from ArduinoBLE source: `HCIClass::sendCommand` busy-polls up to 1 s for the HCI response, which is produced by Cordio's `bleLoop` thread at `osPriorityNormal`; the higher-priority app thread starved it. Fix: app thread at `osPriorityBelowNormal`. After the single change: `BLE ev=advertising ... link_ok=1`. |
| Result | FIXED (verified on hardware) |
| Cost | Event queueing latency rose from 0 ms (T2) to 0–3 ms (T5). |

## T5 — Hardware bench: BLE GATT alert/ack with a real host (2026-10-10)

| Field | Value |
|---|---|
| Setup | Nano 33 BLE Rev2, production build (`pio run -e nano33ble`). Host: macOS + bleak (`tests/bench/ble_bench.py`) as BLE central; button events injected over serial. Device rebooted before each scenario. Runs `20261010-224046` (happy path ×20) and `20261010-224448` (full suite). |
| Expected | Alert notified with correct 6-byte packet and acked over BLE; status characteristic reports PENDING/CONFIRMED/ALERT_SENT/ACKNOWLEDGED; no host → bounded retries → ERROR; connected-but-unsubscribed detected as send failure (ArduinoBLE's `writeValue` would report success); ack timeout → retry → recovery; disconnect mid-alert → RETRY(link_down) → redelivery after reconnect; malformed acks (bad length ×2, version, type, zero id) each logged `BLE_ERR` with the right reason and no state change; wrong-id ack rejected; cancel after send notifies a CANCEL packet. |
| Observed | Full suite 7/7 PASS. Happy path 20/20 PASS. |
| Result | PASS |

Happy-path measurements (n=20, run `20261010-224046`):

| Metric | min | median | p95 | max |
|---|---|---|---|---|
| Event queueing latency, device (ms) | 0 | 0 | 2 | 3 |
| Pending window, device (ms, nominal 3000) | 3000 | 3003 | 3009 | 3009 |
| CONFIRMED log line → BLE notification at host (ms) | 19.4 | 25.6 | 28.1 | 29.0 |
| BLE ack write → ACKNOWLEDGED log line at host (ms) | 29.8 | 37.5 | 41.5 | 43.3 |

Failure-path measurements (single runs, `20261010-224448`): no host, hold → ERROR 7010 ms (nominal 7000); ack timeout 5008 ms; host disconnect → device RETRY 33 ms; disconnect → alert redelivered 2077 ms (attempt 2). In an earlier run reconnection took longer and redelivery landed on attempt 3 of 3 (4077 ms) — see notes.

Notes:
- Host latencies are end-to-end on the host clock (USB CDC + macOS BLE stack + connection interval), not on-air timings.
- The first 20× run had 1 FAIL (`pending_window=2998`): the bench measured from the PENDING line's print time instead of the event timestamp the firmware counts from (the line carried `lat_ms=3`). Bench fixed (`event_ms()`), run discarded, re-run 20/20.
- Redelivery after a disconnect depends on the host reconnecting within the remaining retry budget (2 × 2 s backoff). Observed attempt 2 and attempt 3 across runs; a slower host would reach ERROR. Candidate improvement: retry immediately on LinkUp.
- No pairing/encryption: acceptable for this prototype, not for a real product.

## T2b — Serial bench regression after BLE changes (2026-10-10)

| Field | Value |
|---|---|
| Setup | `nano33ble_sim` build (app thread now BelowNormal), `tests/bench/bench.py`, run `20261010-224324`. |
| Observed | 8/8 PASS; all timeouts within +4 ms of nominal (measured from event timestamps). |
| Result | PASS |
