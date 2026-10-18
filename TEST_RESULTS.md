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

## T9 — V1.5 regression: BLE bench with a real host, serial bench (2026-10-17)

| Field | Value |
|---|---|
| Setup | Same board, firmware `1b5c10a`. BLE: production build (image SHA-256 `b93f111d…a19a`), Mac + bleak as BLE central (`tests/bench/ble_bench.py`), button events injected over serial. Serial: `nano33ble_sim` build, `tests/bench/bench.py`; the production build was flashed back afterwards and produced the same image hash. Console `hold` posts PRESSED+HELD with no release, so these runs exercise the never-released path: the window is timed from HELD (`PENDING_MS = 5000`). Runs `20261017-211515` (BLE full suite), `20261017-211750` (BLE happy path ×20), `20261017-212210` (serial suite). |
| Expected | Same scenarios and pass criteria as T5 and T2b, with the 5 s window. |
| Observed | BLE full suite 7/7 PASS, happy path 20/20 PASS, serial suite 8/8 PASS. |
| Result | PASS |

BLE happy path (n=20, run `20261017-211750`; p95 nearest-rank):

| Metric | min | median | p95 | max |
|---|---|---|---|---|
| Event queueing latency, device (ms) | 0 | 1 | 6 | 7 |
| Pending window, device (ms, nominal 5000) | 5000 | 5007 | 5011 | 5012 |
| CONFIRMED log line → BLE notification at host (ms) | 3.9 | 27.7 | 31.8 | 32.8 |
| BLE ack write → ACKNOWLEDGED log line at host (ms) | 29.5 | 36.9 | 42.3 | 42.9 |

BLE failure paths (single runs, `20261017-211515`): no host, hold → ERROR 9017 ms (nominal 9000 = 5000 + 2 × 2000); ack timeout 5001 ms; retry → acknowledged 2051 ms; host disconnect → device RETRY 74 ms (host clock); disconnect → alert redelivered 4109 ms, on attempt 3 of 3; malformed acks rejected; cancel after send → CANCEL notify 30 ms after the console `press` command (host clock).

Serial suite (`20261017-212210`): pending window 5001 ms, ack display 3001 ms, cancel display 1001 ms, ack timeout 5001 ms (×3), retry backoff 2000 ms (×2), hold → ERROR with link down 9004 ms (nominal 9000), link drop → resent 2003 ms; ping RTT 1.86–2.02 ms (n=20).

Notes:
- Compared with T5, latencies are unchanged within run-to-run spread. The controller change in V1.5 (drain the queue before the Tick) showed no regression; the race it closes was not provoked here.
- Redelivery after disconnect again landed on the last attempt (3 of 3), as in one T5 run. The retry budget after a disconnect is tight; "retry immediately on LinkUp" remains a candidate improvement.
- An earlier attempt (`20261017-211349`) is invalid and not counted: an operator's `button_watch.py` was still holding the serial port, so the two programs split the serial input and the bench missed lines, then failed at a reboot.
- iPhone not tested; the Mac stands in for the gateway.

## T10 — Retry on reconnect: no attempts spent while the host is not ready, 20 s delivery budget (2026-10-17)

| Field | Value |
|---|---|
| Setup | Same board. Firmware: V1.5 plus the retry change (production image SHA-256 `d9f03c26…5c81`). Host tests `pio test -e native`; BLE bench (Mac + bleak); serial bench on the sim build. Runs `20261017-213113` (intermediate version, see below), `20261017-220057` (disconnect ×10), `20261017-220343` (BLE full suite), `20261017-220550` (serial suite). |
| Change | LinkUp is posted when the host subscribes to Alert (was: on connect). The transport checks connected+subscribed before every send and reports NotReady without transmitting; a NotReady probe refunds the attempt. RETRY resends immediately on LinkUp. New `delivery_timeout_ms = 20000` from the first CONFIRMED ends an unacknowledged alert in ERROR (`delivery_timeout`). Packet format, UUIDs and state values unchanged. |
| Expected | A disconnect mid-alert is redelivered as attempt 2 once the host is back; no host or no subscription → nothing transmitted, `attempt=0`, ERROR after 20 s; three unacked real sends still end by the attempt limit (19 s < 20 s); no other regression. |
| Observed | Host tests 85/85. Disconnect mid-alert 10/10 PASS, all redelivered on attempt 2, disconnect → redelivered 1528–3749 ms (avg 2357, host clock). BLE full suite 7/7: no host → ERROR 25014 ms after PENDING (5000 + 20000), `attempt=0`; not subscribed → ERROR `delivery_timeout`, no notification delivered; ack timeout path unchanged (5000 ms). Serial suite 8/8: link down → first CONFIRMED to ERROR 20000 ms with `attempt=0`; link drop → resent 5 ms after `link up`; ack timeouts 5001 ms, backoffs 2000 ms, three unacked attempts end `ack_timeout`. |
| Result | PASS |

Notes:
- Intermediate version (resend on LinkUp only, run `20261017-213113`): 8/10. The resend itself worked (2–8 ms after subscription), but backoff ticks that fell between reconnect and subscription were still spent with `not_subscribed`; in the 2 failures attempt 3 was spent 550 ms before the subscription and 55 ms before the reconnect. That led to the not-ready refund and the time budget.
- User-visible trade-off: with no phone at all, the red ERROR light now comes 20 s after the first send instead of about 4 s (9 s after the hold instead of 25 s). 20 s is a prototype value chosen to cover the ~5 s reconnects seen here with margin; it is not validated, and iPhone background reconnects may need a different value.
- After the serial bench, flashing the production build back failed twice ("No device found" from the bootloader after the 1200-baud touch). A manual double-tap reset by the operator recovered it; the image was then written and verified with bossac and the console confirmed the production build (`link` → `not_simulated`).

## T11 — Repeated-cycle reliability: BLE happy path ×100 (2026-10-17)

| Field | Value |
|---|---|
| Setup | Firmware `9dd21bb` (production image SHA-256 `d9f03c26…5c81`), Mac + bleak as BLE central, `ble_bench.py --only happy_path --repeat 100`, run `20261017-225159`, about 16 min. The Mac was downloading a macOS installer during the run. |
| Expected | 100/100 PASS; no event-queue drops; no error lines. |
| Observed | 100/100 PASS. `q_dropped=0` in all 108 heartbeats; no `BLE_ERR`, `BTN_ERR` or overflow lines. Pending window median 5006 ms (p95 5011, max 5012); event queueing latency median 1 ms (max 7); CONFIRMED log → notification at host median 30.6 ms (p95 61.0, max 96.4); ack write → ACKNOWLEDGED median 34.9 ms (p95 42.5, max 63.8). |
| Result | PASS |

Notes:
- The bench reboots the board before every scenario, so this shows 100 consecutive reliable alert cycles, not long uptime. A continuous run without reboots is still to do.
- Notification latency p95 (61 ms) is higher than in T9 (31.8 ms, n=20). The concurrent download may explain it; not established. Well inside the 5 s ack window either way.

## T12 — Host unit tests: iPhone gateway logic (2026-10-18)

| Field | Value |
|---|---|
| Setup | `ios/ReachGateway` (new, SwiftUI, iOS 17.0 deployment target), Xcode 27.1, `xcodebuild test` on the iOS 27.1 simulator. Pure logic only: `AlertProtocol`, `EventMatcher`, `UploadOutcome`/`UploadRequest`. No Bluetooth, no network. |
| Expected | Ack bytes match the firmware layout; malformed alert/status packets rejected; attempt 1 always starts a new event (alert_id restarts at 1 after a board reset); attempt > 1 reuses the newest matching event within 10 min; event_id fits the backend character set and 128-char limit; 201/200 succeed, 401/403/409/413/422 stop, other codes retry; backoff 2, 4, 8 … capped at 60 s; upload body has exactly the three contract fields. |
| Observed | 15/15 tests passed, no compiler warnings. |
| Result | PASS |

## T13 — First real iPhone run: button → BLE → App → backend (2026-10-18)

| Field | Value |
|---|---|
| Setup | iPhone SE (3rd generation, iPhone14,6), iOS 18.1.1, ReachGateway debug build installed from Xcode 27.1, App in the foreground. Board on USB to the Mac for power and serial; firmware already on the board (not re-flashed for this run, image not re-checked). Backend: the teammate's device inbox through the existing ngrok upload URL. |
| Expected | App finds `WEA-Nano33`, subscribes to Alert; a hold produces one event stored on the phone, then an Ack; the upload returns 201; a retry of the same alarm reuses its event_id. |
| Observed | App connected and subscribed. Hold on the middle button produced one `Medical` event, `alert_id 1`, `attempt 1`, event_id `nano-01-87a65e40-14e8-47ac-8dc2-ceaed18e4078`. No retransmission reached the App (attempt stayed 1), consistent with the Ack being accepted; the serial log was not captured for this hold, so ACKNOWLEDGED on the board is not directly shown. First upload: 401, because a read-only dashboard credential had been entered instead of the device credential. After entering the device credential, Retry sent the same event_id and the App showed 201. Wiring re-check (`button_watch.py`, run `20261018-010954`): short presses left/middle/right gave `btn=1 EMERGENCY` (D2), `btn=2 MEDICAL` (D3), `btn=3 FAMILY` (D4), one press and one release each, no extra edges, no alert from short presses. |
| Result | PASS for BLE receive, store-then-ack and upload (201 after the credential fix). Not yet done: a D2 Emergency end-to-end run, the teammate's backend-side check of this event_id, and contact acceptance (the help tunnel was down: `no tunnel`). |

Notes:
- The press was on D3, which is correct for `Medical`; the earlier confusion was about which physical button is D2 (the left one).
- 401 is a stop condition in the App by design (retrying cannot fix a wrong credential); the manual Retry after fixing it is the intended recovery and kept the original event_id.

## T14 — First full chain: button → BLE → iPhone → backend → contact accepted (2026-10-18)

| Field | Value |
|---|---|
| Setup | Same as T13 (iPhone SE 3rd gen, iOS 18.1.1, App in the foreground, device credential corrected). Contact page on the Mac browser through the teammate's temporary help tunnel, signed in with the contact credential. |
| Expected | One event in the App with a 201 upload; the same event_id visible on the contact page; "I can help" moves it to Contact accepted. |
| Observed | Hold on the left button (D2): App event `Emergency`, `nano-01-c41ea862-09b5-4b2b-8379-8275b2e99fdb` (type read from the App by the operator). Contact page listed request `b2e99fdb` (device `nano-01`) recorded 01:33:39, history "Recorded from the device inbox" 01:33:40; after "I can help" it showed Contact accepted at 01:58:30 ("Separate contact access"). The T13 event (`…d18e4078`) was also listed, recorded 01:32:52 (the Retry after the credential fix), state Recorded by backend. |
| Result | PASS for the chain as observed. The backend does not store alert type yet, so the contact page shows only "Device event". |

Notes:
- Contact accepted means a contact took responsibility, not that help arrived; the contact page states this.
- The temporary help tunnel (localhost.run) dropped three times during the session (`no tunnel`). The ngrok upload URL stayed reachable. For the demo, opening the contact page locally on the backend host avoids that dependency.

## T15 — iPhone gateway reliability: offline backend, app relaunch, board reset, lost ack (2026-10-18)

| Field | Value |
|---|---|
| Setup | Same phone and App as T13–T14, plus a test toggle "Skip the next ACK". Board serial logged on the Mac for the whole run (`20261018-020548_gateway_reliability_serial.log`). During the run the teammate's upload endpoint went offline (ngrok `ERR_NGROK_3200`, HTTP 404), so every upload in this run stayed pending. |
| Expected | A: with the backend unreachable, the App still stores and acks, and keeps retrying with the same event_id. B: killing and relaunching the App keeps a pending event and resumes its upload without a duplicate. C: after a board reset, `alert_id 1` becomes a new event. E: if one ack is lost, the wearable's attempt 2 reuses the original event_id and is acked. |
| Observed | A: D2 hold, board `BLE_TX ALERT id=3 attempt=1` at 02:08:04.705, `BLE_RX ACK id=3` 62 ms later, ACKNOWLEDGED. App event `…d41518af5a89` showed "Sending to backend… (HTTP 404)" and kept retrying (tries 6, 7, 8 seen). B: App killed from the app switcher; board logged disconnect 02:11:44 and reconnect + Alert subscription 02:11:53–54. After relaunch the same event was listed, still pending, tries went 6 → 7, no new event. C: two board resets (02:12:44, 02:13:01); the App reconnected and subscribed about 1 s after each boot. Next D2 hold: `id=1 attempt=1`, ack after 85 ms; App created a new event `…ed23be6b44fd` (`alert_id 1`), separate from all earlier ones. E: toggle on, D2 hold: attempt 1 sent 02:16:45.566, `ack_timeout` 5.0 s later, attempt 2 sent 02:16:52.572, ack 41 ms later, ACKNOWLEDGED. App showed one event `…0a0ee089d50f`, `alert_id 2`, `attempt 2`. |
| Result | PASS for A (phone side), B, C and E. Not yet shown: that the pending events upload once with their original event_ids when the backend is back (second half of A and B). |

Notes:
- Skipped on purpose: cancel inside the 5 s window (nothing reaches the phone; covered by T6–T8) and walking out of range (wearable redelivery after reconnect is covered by T10; the App handles that attempt the same way as in E).
- The App does not re-upload an event that already got 201/200, so a retransmission after a successful upload produces no second backend request (not exercised here, since the backend was offline).

## T16 — iPhone gateway with the phone locked (2026-10-18)

| Field | Value |
|---|---|
| Setup | ReachGateway with `bluetooth-central` background mode, Core Bluetooth state restoration, a local notification per new alert, and a background task around uploads. iPhone SE (3rd gen), iOS 18.1.1. Wearable on a USB power bank (no serial log). Backend upload endpoint still offline (HTTP 404). |
| Expected | With the App connected and the phone locked, a D2 hold is stored and acked without a retransmission, a notification appears on the lock screen, and the upload keeps retrying with the same event_id. |
| Observed | Phone locked about 10 s before the hold. The lock screen showed the "Reach: Emergency alert" notification (reported by the operator). After unlocking, the App listed `Emergency`, `alert_id 2`, `attempt 1`, `nano-01-b2793be5-74d4-4dac-b0d2-17e80e322df5`, "Sending to backend… (HTTP 404)", tries 4. Attempt 1 means the ack written while locked was accepted before the wearable's 5 s ack timeout. |
| Result | PASS for receive, store, ack and notify while locked. Not shown: upload success while locked (backend offline), and relaunch by iOS after it terminates the App (state restoration is implemented but not exercised). |

Notes:
- iOS gives a background task limited time; if the upload has not succeeded by then, retries continue the next time the App runs. The event stays saved meanwhile.
- If the user force-quits the App from the app switcher, iOS does not relaunch it for Bluetooth events.
- The lock-screen notifications were removed afterwards: the gateway is a home device that nobody watches, and notifying family is the backend's job. Background receive and ack are unchanged.

## T17 — Host unit tests: gateway store-then-ack (2026-10-18)

| Field | Value |
|---|---|
| Setup | Alert intake moved out of `Gateway.swift` into `AlertIntake.swift` behind two small protocols (`EventStoring`, `AckSending`); `EventStore` and `BLEManager` conform unchanged. `AlertIntakeTests` uses a fake store and fake BLE link that write to one shared journal, so the order of save and ack is checked. `xcodebuild test` on the iOS 27.1 simulator (Xcode 27.1), and in GitHub Actions CI. |
| Expected | New alert: saved, then acked. Save fails (new or retransmission): no ack. Retransmission: same event_id, acked again, `lastAttempt` updated. Link down: event kept, the later retransmission reuses it. Test tool skip: saved, no ack. Board reset (alert_id 1, attempt 1 again): new event. |
| Observed | 22/22 tests passed (15 existing + 7 new). Mutation check: with the ack moved before the save, 6 of the 7 new tests failed; code restored afterwards. |
| Result | PASS (host logic only; no new hardware or iPhone run) |

