# Reach

Reach is an educational prototype of a wearable emergency-alert controller on
an Arduino Nano 33 BLE Sense Rev2 (nRF52840, Cortex-M4, Mbed OS). It is **not** a medical device
and does not contact emergency services itself: it raises alert events over BLE,
and a host application decides what to do with them (call, message, etc.).

Status and measured results: see [TEST_RESULTS.md](TEST_RESULTS.md).

## End-to-end system (ReachGuard)

The wearer only has the pendant; they are not expected to own or use a
smartphone. A home gateway relays alerts to the internet. In this prototype the
gateway is an iPhone running `ios/ReachGateway`, standing in for a spare phone
kept plugged in at home (later: a small dedicated hub, or a cellular pendant
that needs no gateway). Family members respond on the contact page; a primary
family member (the "owner" role, e.g. the wearer's son) follows every step on
the Dashboard.

```
wearable (this repo) ──BLE──► home gateway: iPhone app (ios/ReachGateway, this repo)
                                   │ HTTPS, device credential
                                   ▼
                         backend device inbox + help service
                         (github.com/Cyberchopin/reachguard)
                                   │
                                   ▼
                     contact page ("I can help") and Dashboard
```

First full run on 2026-10-18 (TEST_RESULTS.md T14): a D2 Emergency hold went
from the wearable over BLE to a real iPhone, was uploaded (HTTP 201), and the
contact accepted it on the contact page. "Contact accepted" means someone took
responsibility; it does not mean help arrived.

## Architecture

```
GPIO ISR ───────┐
serial console ──┼─► AppEventQueue (rtos::Mail, ISR-safe) ─► app thread
BLE callbacks ───┘                                            ├─ StateMachine (pure, host-tested)
                                                              └─ AlertTransport: BleLink | SimulatedLink
```

- `lib/app` — event types and the alert state machine (transition table in `state_machine.h`)
- `lib/button_logic` — debounce and deliberate-hold detection
- `lib/protocol` — BLE packet encoding/validation
- `src/app` — event queue and the controller thread
- `src/drivers` — buttons (D2 Emergency, D3 Medical, D4 Family), status LED, BLE GATT transport
- `src/services` — diagnostics log, serial console, simulated link

The app thread is the only thread that touches the state machine and ArduinoBLE,
and it runs at `osPriorityBelowNormal` so Cordio's `bleLoop` thread can preempt it
(see TEST_RESULTS.md T4 for why this matters).

## BLE GATT service

Advertised name `WEA-Nano33`. No pairing (prototype only).

| Characteristic | UUID | Properties | Payload |
|---|---|---|---|
| Service | `8f1d0001-6c2a-4b7e-9e3d-5a4c3b2a1f00` | | |
| Alert | `8f1d0002-…` | read, notify | 6 B: version, type (1 ALERT / 2 CANCEL), alert_id (u16 LE), alert type (1 Emergency, 2 Medical, 3 Family), attempt |
| Ack | `8f1d0003-…` | write | 4 B: version, 0x81, alert_id (u16 LE) |
| Status | `8f1d0004-…` | read, notify | 6 B: version, state, alert_id (u16 LE), alert type, attempts |

Notifications are not delivery-confirmed by BLE, so an alert counts as
delivered only when the host writes an Ack. Unacknowledged alerts are retried
(3 real transmissions max, 5 s ack timeout, 2 s backoff) within a 20 s delivery
budget from the first send, and then end in a sticky ERROR. Before every send
the firmware checks that the host is connected **and** subscribed to Alert; if
not, nothing is transmitted and no attempt is used. The moment the host
subscribes, a pending retry is sent without waiting for the backoff.

Requirements and limits the host (iPhone gateway) must respect:

- **Deduplicate by `alert_id`.** Every retry of one alert reuses its
  `alert_id` (only `attempt` changes, 1..3), and a late ack from an earlier
  attempt still counts. The host must create at most one event per
  `alert_id`. IDs live in RAM and restart at 1 after a board reset, so dedup
  must be scoped to the current connection or boot, not kept forever.
- **CANCEL is not acknowledged.** It is sent once, best effort. A successful
  BLE send only means the packet was handed to the BLE stack; it does not mean
  the host received it or stopped the alert. If the send fails the firmware
  logs `cancel_delivery=unknown`. There is no CANCEL ack in this protocol
  version.
- **An ack only means the host accepted the alert**, not that a caregiver saw it.
- **A late ack after ERROR is not accepted.** The firmware stays in ERROR and
  logs `late_ack_after_error=1` with the alert id.

## Build and test

```sh
pio test -e native                    # host unit tests
pio run -e nano33ble -t upload        # BLE firmware
pio run -e nano33ble_sim -t upload    # serial-only build with a simulated link

python3 -m venv .venv && .venv/bin/pip install -r tests/bench/requirements.txt
.venv/bin/python tests/bench/ble_bench.py   # needs the nano33ble build
.venv/bin/python tests/bench/bench.py       # needs the nano33ble_sim build
```

Serial console commands (115200 baud): `ping`, `reboot`, `hold <1-3>`, `press`,
`ack <id>`, `link up|down` (sim build only).

## iPhone gateway app

`ios/ReachGateway` is a SwiftUI app (iOS 17+) that connects to `WEA-Nano33`,
stores each alert on the phone, writes the BLE Ack, and uploads a
`help_request` to the backend, retrying with the same `event_id` until it gets
201/200. A retransmission (`attempt` > 1) reuses its event; `attempt` 1 always
starts a new one, so a board reset cannot merge alarms. 401/403/409/413/422
stop retrying and show the reason.

```sh
brew install xcodegen
cd ios/ReachGateway && xcodegen generate     # project.yml -> ReachGateway.xcodeproj
xcodebuild test -project ReachGateway.xcodeproj -scheme ReachGateway \
  -destination 'platform=iOS Simulator,name=<any iPhone>'   # 15 unit tests
```

Run on a phone: open the project in Xcode, pick your team under Signing &
Capabilities, select the iPhone, Run. With a free Apple ID the install expires
after 7 days. In the app's settings enter the backend base URL (without
`/api/device-events`) and the device credential; the credential is kept only in
the iOS Keychain and is never committed. "Test tools → Skip the next ACK"
forces one retransmission for testing.

## Validation summary

| Area | Result | Record |
|---|---|---|
| State machine, debouncer, protocol (host) | Unit tests pass | T1, T3 |
| Real buttons, LED, cancel, simultaneous holds | Pass on hardware | T6–T8 |
| BLE alert/ack with a Mac host, retry on reconnect | Pass | T5, T9, T10 |
| BLE happy path ×100 | 100/100 | T11 |
| iPhone gateway unit tests | 15/15 | T12 |
| First real iPhone run, upload 201 | Pass | T13 |
| Full chain to "Contact accepted" | Pass | T14 |
| Backend offline, app relaunch, board reset, lost ack | Pass on the phone side | T15 |
| Gateway phone locked: receive, ack | Pass | T16 |

## Prototype findings

- **Three buttons may be too many.** While testing, the developers themselves
  could not always tell which button was which. For an older person, choosing
  between Emergency, Medical and Family in an emergency is a burden. The next
  iteration explores a single button; the firmware keeps three-button support.
- **One contact is not enough.** If the only contact misses the alert, nothing
  else happens. Contact rotation (notify close family first, alternating, then
  others, round after round until someone accepts) is implemented in the
  backend as an open pull request (Cyberchopin/reachguard#5), tested with
  synthetic data, not yet with the wearable.

## Limitations

- Not a medical device. It never contacts emergency services; demos only notify
  team members.
- The gateway app keeps receiving and acking with the phone locked (T16), but
  iOS limits background time: a failed upload may wait until the app runs
  again, and a force-quit app is not relaunched for Bluetooth events.
- Family members are not pushed: the contact page and Dashboard must be open to
  see a new turn (they beep and, where the browser allows, show a notification).
  A family app with push notifications is future work.
- The gateway must be within BLE range of the wearable (roughly the same room
  or the next one). If it is not, the wearable retries and then shows ERROR;
  the alert is not sent.
- No voice. The wearable has no speaker and no cellular or Wi-Fi radio; a
  cellular voice module is the likely path for a later version.
- The backend does not store the alert type yet, so the contact page shows only
  "Device event".
- Timing parameters (1.5 s hold, 5 s cancel window, 5 s ack timeout, 3
  attempts, 20 s delivery budget) are prototype defaults, not validated with
  older users.
