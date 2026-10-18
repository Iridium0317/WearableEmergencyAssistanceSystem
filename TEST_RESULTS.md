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

## T6 — Real buttons: wiring, debounce, cancel lockout, consumed press (2026-10-17)

| Field | Value |
|---|---|
| Setup | Nano 33 BLE Sense Rev2, `nano33ble` build, no BLE central connected (sends fail → ERROR is the expected end state). Three 4-pin tactile switches, D2/D3/D4 → switch → GND, internal pull-ups. `tests/bench/button_watch.py`, operator presses by hand. Part 1 on firmware `33433c8` (plus comment-only edits); part 2 on `cee9fdd` (image SHA-256 `00ed5fa6…df73`, rebuilt from that commit bit-for-bit). Raw output was pasted into the dev session, not saved to `results/`. |
| Expected | Each button maps to its alert type; one PRESSED/RELEASED per press with no duplicates; short presses never alert; hold → PENDING; cancel in PENDING sends nothing, cancel after send attempts a CANCEL; (part 2) own-button re-press within 300 ms of its release is rejected; a cancelling press's hold never starts a new alert. |
| Observed | Part 1: D2/D3/D4 → EMERGENCY/MEDICAL/FAMILY, no cross-talk on the shared ground rail; every press gave exactly one PRESSED/RELEASED (mostly 2 raw edges per press, one press with 5 edges filtered); max ISR→task 92 µs. PENDING cancel and RETRY cancel both worked; 3 failed sends with one alert ID, then ERROR. Part 2: re-presses 168 ms and 140 ms after a release → `SM_REJECT reason=cancel_lockout`, alert unaffected (CONFIRMED 3001 ms after PENDING); Family pressed while Medical was held → CANCELLED, then Family's HELD (state already IDLE) → `SM_REJECT reason=hold_after_consumed_press`; failed CANCEL logged `cancel_delivery=unknown`. |
| Result | PASS |

Findings that drove V1.5 (T7):
- On `33433c8`, a single clean 48 ms reopen of the held button (1 raw edge each way, so not contact bounce) was taken as a cancel. Debouncing cannot filter this; it motivated the 300 ms lockout.
- With the 3 s window timed from HELD, the operator (who knew the rules and watched the screen) missed the cancel window twice, releasing 2.45 s and 3.3 s after HELD. It motivated timing the window from release.
- The lockout re-arms on every release of the own button, so the 140 ms re-press above was rejected although it came 392 ms after the first release. Kept on purpose (tremor protection outweighs fast same-button re-tapping; other buttons always cancel).

## T7 — V1.5 hardware validation: LED, 150 ms hold gap tolerance, 5 s release-timed window (2026-10-17)

| Field | Value |
|---|---|
| Setup | Same board and wiring as T6, firmware `1b5c10a` (`nano33ble`, image SHA-256 `b93f111d…a19a`), no BLE central. `tests/bench/button_watch.py`; operator presses by hand following a written script. Logs: `results/20261017_v1.5_run1_buttons_watch.log`, `results/20261017_v1.5_run2_guided_buttons_watch.log` (copied from the terminal output pasted into the dev session). |
| Expected | LED: IDLE off, PENDING blue fast blink, sending/retry blue steady, ERROR red slow blink (sticky), CANCELLED white ~1 s. Lifts under 150 ms during a hold do not restart it; longer lifts do. Window = 5 s from the own button's debounced release, even after a long hold. |
| Observed | See tables below. |
| Result | PASS for the three items, within the limits listed below. |

LED (operator, visual, not timed): all five patterns matched the expected ones.

5 s window, measured from the RELEASED event timestamp the firmware counts from:

| Alert | HELD → release (ms) | Release → CONFIRMED (ms) | HELD + 5000 would have sent (ms earlier) |
|---|---|---|---|
| id=1 (run 1) | 544 | 5002 | 544 |
| id=3 (run 1) | 1102 | 5009 | 1102 |
| id=5 (run 2) | 1106 | 5009 | 1106 |
| id=7 (run 2, held 3.9 s in total) | 2425 | 5008 | 2425 |

Overshoot is within the 10 ms controller tick. Each alert then retried 3× with one ID and ended in ERROR.

Hold gap tolerance (lift durations from ISR edge timestamps, 1 ms resolution):

| Lift (ms) | Count | Outcome |
|---|---|---|
| 58, 70, 88, 101, 119 | 5 | bridged (`bridged=1`); hold progress kept. id=8 reached HELD exactly 1500 ms after the original press start with a 70 ms lift inside it |
| 166, 273, 366 | 3 | hold restarted (reported as `lift_ms` on the next PRESSED) |

No lift under 150 ms restarted a hold and none over 150 ms was bridged. Other observations: a 56 ms wobble right after HELD was rejected by the 300 ms lockout (`cancel_lockout`), as were re-presses at 260 and 266 ms; same-button cancels at 1809 ms and 6440 ms after release worked; short presses (≥ 15) never alerted.

Limits:
- The 150 ms boundary is bracketed by manual data between 119 and 166 ms; the exact edge (e.g. 149/150/151 ms) was not tested on hardware. That needs a controlled input (e.g. D5 driven open-drain into D2).
- Not tested on hardware: the never-released fallback (alert at HELD + 5 s), two simultaneous cancel presses (CANCELLED consumption), the controller draining the queue before the Tick, and any of V1.5 with a real BLE central or iPhone.
- On the first press after boot, `lift_ms` shows uptime, since there is no earlier release (cosmetic).
- All timings are prototype defaults, not medically or ergonomically validated.

## T8 — V1.5 hardware: never released, two-finger cancel, simultaneous holds, reset during PENDING (2026-10-17)

| Field | Value |
|---|---|
| Setup | Same board, wiring and firmware as T7 (`1b5c10a`), no BLE central. Operator presses by hand following a written script. Logs: `results/20261017_v1.5_run3_guided_round2_buttons_watch.log`, `results/20261017_v1.5_run4_reset_during_pending_buttons_watch.log`. |
| Expected | A button never released still sends at HELD + 5 s; two buttons pressed together to cancel never start a new alert while held; two buttons held together give exactly one alert (first HELD wins); reset during PENDING: documented behaviour, not a pass/fail target. |
| Observed | See below. LED patterns matched in all cases (operator, visual). |
| Result | PASS for the first three; the fourth confirms a known limitation. |

- **Never released.** Family held 13 s: CONFIRMED 5009 ms after HELD while still held, 3 attempts, ERROR, release afterwards had no effect. Two later alerts held through the deadline: CONFIRMED 5001 ms and 5011 ms after HELD.
- **Two-finger cancel (CANCELLED consumption, race A).** Medical alert in RETRY; Emergency pressed (cancel, `cancel_delivery=unknown`), Medical pressed 75 ms later, landing in CANCELLED. Both held: their HELDs (state IDLE by then) were both rejected with `hold_after_consumed_press`.
- **Simultaneous holds.** Medical+Family (Medical 108 ms first) → one MEDICAL alert, Family's HELD `alert_in_progress`. Emergency+Medical (Emergency 93 ms first) → one EMERGENCY alert, Medical rejected. A same-millisecond tie (D2 > D3 > D4 by pin order) cannot be produced by hand and is covered by host tests only. Two simultaneous short presses produced no alert.
- **Reset during PENDING.** Board reset button pressed about 3 s into the 5 s window (host clock). After reboot the state was IDLE with the LED off: the pending alert was lost without any indication to the user. The next alert reused `id=1`.

Limitations confirmed here:
- Alert state lives in RAM only. A reset (button, brown-out, crash) during PENDING/CONFIRMED/RETRY drops the alert silently. Candidate mitigations, not implemented: persist the active alert across reset, signal a lost alert at boot, add a boot/session counter to the protocol (protocol change, needs approval).
- Alert IDs restart at 1 after reset, so host dedup must be scoped per connection/boot (README).
