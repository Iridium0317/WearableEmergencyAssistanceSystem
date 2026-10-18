# Reach firmware V1.5: summary and handoff

Status as of 2026-10-17, `main` at `9dd21bb`. This is a prototype, not a
certified medical or emergency device. Every timing below is a prototype
default, not medically or ergonomically validated.

## What exists

Arduino Nano 33 BLE Sense Rev2 firmware (C++, Mbed OS / RTOS, ArduinoBLE)
for three emergency buttons that raise alerts over BLE to a phone, which
acknowledges them.

| Layer | Where | Host-tested |
|---|---|---|
| GPIO edges → debounce, hold, tremor gap tolerance | `src/drivers/buttons.*`, `lib/button_logic/` | yes (`test_button_debouncer`, `test_edge_ring`) |
| Alert lifecycle state machine | `lib/app/state_machine.*` | yes (`test_state_machine`) |
| BLE GATT transport, ack, readiness check | `src/drivers/ble_link.*`, `lib/protocol/` | protocol yes (`test_alert_protocol`) |
| Status LED | `lib/feedback/` (patterns), `src/drivers/status_led.*` (pins) | patterns yes (`test_status_pattern`) |
| App thread, event queue | `src/app/` | no (Mbed-dependent) |

History: `454906a`–`629b442` (2026-10-10) built bring-up, the state machine
and BLE; `33433c8` added the GPIO button driver. The commits from `cee9fdd`
to `9dd21bb` (2026-10-17) are the work summarised here.

## How the wearer uses it

| Action | Result |
|---|---|
| Hold any button 1.5 s | Alert of that button's type starts (D2 Emergency, D3 Medical, D4 Family). LED: blue fast blink |
| Short press | Nothing |
| Lift the finger for less than 150 ms during the hold | Hold continues (tremor tolerance) |
| Let go after the alert started | Does not cancel. A 5 s cancel window starts at the release |
| Never let go (e.g. lying on the button) | Alert still goes out 5 s after the hold completed |
| Press any button within the window | Cancelled. LED: white for 1 s |
| Re-touch the same button within 300 ms of letting go | Ignored (wobble guard); other buttons always cancel |
| Keep holding the button used to cancel | Never starts a new alert; release and hold again for that |
| Nothing | Alert is sent. LED: blue steady while sending, green on phone ack, red slow blink if not delivered |

The button types are labels only; what each one means is the phone's
decision. Open product question: Emergency and Medical overlap (see
"Open questions").

## Delivery rules

- Up to 3 real transmissions, 5 s ack timeout each, 2 s apart, all with the
  same `alert_id` (packet `attempt` = 1..3).
- Before every send the firmware checks that the phone is connected and
  subscribed to the Alert characteristic. If not, nothing is transmitted and
  no attempt is used.
- When the phone subscribes, a pending retry is sent at once.
- 20 s after the first send without an ack, the alert ends in ERROR
  (`delivery_timeout`). ERROR stays until the next hold.
- A cancel after sending sends a CANCEL packet once, best effort. There is no
  CANCEL ack, so the device never knows whether the phone cancelled
  (`cancel_delivery=unknown` when the send fails).
- An ack only means the phone accepted the alert, not that a person saw it.
  An ack arriving after ERROR is logged (`late_ack_after_error=1`) and does
  not turn the failure into success.

## What the iPhone gateway must do

Protocol details (UUIDs, packet layouts) are in `README.md`. Unchanged since
`629b442`; the gateway can rely on them.

1. Scan for `WEA-Nano33`, connect, and **subscribe to Alert** (and Status if
   wanted). Subscribing is what the device treats as "ready".
2. On an ALERT packet, write an Ack with the same `alert_id` promptly. The
   device waits up to 5 s per attempt.
3. **Deduplicate by `alert_id`**: one event per id, retries carry the same id.
   IDs restart at 1 when the board resets, so scope dedup to the current
   connection or boot.
4. On a CANCEL packet for an `alert_id`, stop that alert. A missing CANCEL is
   possible; do not assume one arrives.
5. Reconnect automatically after a disconnect and re-subscribe. The device
   resends on subscription, but gives up 20 s after its first send.
6. Never place real emergency calls during demos.

## Validation

All in `TEST_RESULTS.md`, raw logs in `tests/bench/results/`.

| Record | Covers | Result |
|---|---|---|
| Host tests | 85 cases across 5 suites | 85/85 |
| T0–T5, T2b (2026-10-10) | Bring-up, state machine and BLE before buttons | PASS (T4 documents a fixed failure) |
| T6 | Real buttons, debounce, cancel lockout, consumed press | PASS |
| T7 | LED, 150 ms gap tolerance, 5 s release-timed window | PASS |
| T8 | Never released, two-finger cancel, simultaneous holds, reset during PENDING | PASS; reset loses the alert (limitation) |
| T9 | BLE bench with a Mac as phone (7/7, 20/20), serial bench 8/8 | PASS |
| T10 | Retry on reconnect, not-ready refund, 20 s budget | PASS (disconnect 10/10 on attempt 2) |

Not validated: iPhone end to end, the exact 150 ms boundary (hand tests
bracket it between 119 and 166 ms), long-run stability, a same-millisecond
two-button tie on hardware, and any of this with older users.

## Known limitations and risks

| Limitation | Effect | Candidate fix |
|---|---|---|
| Alert state is RAM only | A reset during an alert drops it silently; IDs restart at 1 | Persist the active alert; signal a lost alert at boot; boot counter in the protocol (protocol change) |
| No CANCEL ack | Phone may keep an alert the wearer cancelled | CANCEL ack in the protocol (needs approval) |
| 20 s delivery budget | With no phone, red light only ~25 s after the hold | Tune once iPhone reconnect times are measured |
| Tremor tolerance | Rapid tapping with lifts under 150 ms for 1.5 s counts as a hold | Validate with users |
| Any button cancels | A stray touch on another button cancels a real alert | Consider a deliberate cancel gesture, with user testing |
| No pairing or encryption | Anyone nearby can connect | Required before real use |
| Board bootloader | Twice needed a manual double-tap reset to accept an upload | Recover as below |

## Open questions for the team

- Emergency vs Medical overlap. Suggested: define the buttons by urgency
  (now / health but not life-threatening / just contact me), or reduce to two
  buttons. The firmware does not care; CLAUDE.md currently says three.
- What the phone does for each type, who gets notified, and whether there is
  a backend.
- Whether a cancel should need a deliberate action instead of any press.

## Build, test, flash

```sh
pio test -e native                  # host tests
pio run -e nano33ble -t upload      # production (BLE)
pio run -e nano33ble_sim -t upload  # serial-only build for tests/bench/bench.py
python tests/bench/button_watch.py  # live button and state log (reconnects after reset)
python tests/bench/ble_bench.py     # Mac as the phone; production build
python tests/bench/bench.py         # serial bench; sim build
```

Only one program may hold the serial port at a time; close `button_watch.py`
before running a bench or uploading. If an upload reports "No device found",
double-tap the board's reset button (LED pulses) and upload again.
