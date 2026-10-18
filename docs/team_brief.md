# Reach — Team Brief

A wrist-worn device with three buttons (Emergency, Medical, Family) that sends a confirmed alert to a phone over Bluetooth; the phone handles calling and texting caregivers.

## The pitch

Older adults who live alone often cannot reach a phone after a fall or sudden illness, and phone-based panic apps assume they can. Our device is a physical button on the wrist: hold it, and a caregiver is called.

**30-second version:** "Grandma falls, her phone is in the kitchen. She holds the red button on her wrist for two seconds. When she lets go, she has five seconds to cancel if it was an accident. Then the wearable sends the alert to the phone over Bluetooth, the app calls her daughter and texts her location, and the wearable shows the alert was received. If the phone is out of range, it retries and tells her the alert did not go through, instead of failing silently."

Three buttons, three audiences: **Emergency**, **Medical** (e.g. a nurse line or doctor), **Family** (non-urgent check-in).

## How it works

The wearable never places calls itself; it raises a confirmed alert, and the phone decides what to do with it.

1. **Hold a button 1.5 s.** Short taps are ignored, so bumping it does not trigger anything. Brief lifts under 150 ms (tremor) do not restart the hold.
2. **5-second cancel window, counted from letting go.** Pressing any button cancels a false alarm before anything is sent; the LED blinks blue while the window runs. If the button is never released, the alert goes out 5 s after the hold.
3. **Alert sent over BLE** to the paired phone: alert type, alert ID, attempt number.
4. **Phone acknowledges.** No ack within 5 s, or Bluetooth drops, means the wearable retries (up to 3 real sends, 2 s apart, resent the moment the phone reconnects). Waiting for a disconnected phone costs no attempt; after 20 s without an ack the wearable shows the alert as not delivered.
5. **Phone app acts:** call and text the caregiver (e.g. via Twilio), share location.
6. **Delivery failed after 3 attempts:** the wearable stays in an ERROR state the user can see, rather than pretending it worked.

Hardware: Arduino Nano 33 BLE Sense Rev2 (nRF52840, Cortex-M4) running Mbed OS, with an onboard IMU we can use for fall detection.

## What already works

The firmware core and BLE link are built and tested on the real board; the physical buttons are wired but not yet confirmed working.

| Piece | Status | Evidence |
| --- | --- | --- |
| Alert state machine (pending, cancel, retry, ack, error) | Working | 39 host unit tests; 8/8 serial bench scenarios on hardware (run with the earlier 3 s window, not yet rerun) |
| BLE service (alert, ack, status) | Working | 7/7 BLE scenarios with a Mac as the phone; 20/20 repeated runs |
| Alert reaches phone after confirm | Working | Median 25.6 ms, p95 28.1 ms (n=20) |
| Phone ack reaches wearable | Working | Median 37.5 ms, p95 41.5 ms (n=20) |
| Disconnect and reconnect mid-alert | Working | Alert redelivered after reconnect, within the 3-attempt budget |
| Physical buttons (D2, D3, D4) | Not verified | Driver runs, but no button press has registered yet; wiring check pending |
| Fall detection (IMU) | Not started | IMU detected on the board only |
| Phone app | Not started | Tests use a Python script as the phone |
| LED or vibration feedback | Not started | |

All numbers come from logged test runs in the repo (`TEST_RESULTS.md`).

## Weekend plan

The biggest gap for a demo is the phone side: today nothing turns an alert into a real call. Four workstreams can run in parallel because the BLE packet format is already fixed (see `README.md`).

| Workstream | What "done" looks like | Owner |
| --- | --- | --- |
| Phone app | Connects to `WEA-Nano33`, subscribes, shows the alert, sends the ack | |
| Caregiver call/SMS | App or small backend triggers a Twilio call and text to a demo number | |
| Firmware | Buttons verified on hardware; LED feedback for pending, sent, acknowledged, error | |
| Fall detection | IMU threshold heuristic that starts the same 5 s cancel window | |
| Demo and pitch | 2-minute live demo script, slides, backup video | |

Suggested order: buttons and phone app first (they make the demo), then Twilio, then fall detection if time allows.

## Decide before we start

- [ ] **Prior work:** the firmware was written before the event. Check LA Hacks rules on pre-existing code and disclose it to judges; if not allowed, the phone app, Twilio and fall detection become the submission.
- [ ] **No real 911 calls.** Demo with a teammate's number; auto-dialing emergency services has legal limits and is not something a prototype should do.
- [ ] **iOS or Android?** iOS apps cannot place a call without the user tapping, so Twilio from a backend is the reliable path on both.
- [ ] **Which track or prize** we are aiming for (health, hardware, social good).
