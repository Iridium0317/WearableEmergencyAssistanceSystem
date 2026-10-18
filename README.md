# Reach

Reach is an educational prototype of a wearable emergency-alert controller on
an Arduino Nano 33 BLE Sense Rev2 (nRF52840, Cortex-M4, Mbed OS). It is **not** a medical device
and does not contact emergency services itself: it raises alert events over BLE,
and a host application decides what to do with them (call, message, etc.).

Status and measured results: see [TEST_RESULTS.md](TEST_RESULTS.md).

## Architecture

```
GPIO ISR (next) ─┐
serial console ──┼─► AppEventQueue (rtos::Mail, ISR-safe) ─► app thread
BLE callbacks ───┘                                            ├─ StateMachine (pure, host-tested)
                                                              └─ AlertTransport: BleLink | SimulatedLink
```

- `lib/app` — event types and the alert state machine (transition table in `state_machine.h`)
- `lib/button_logic` — debounce and deliberate-hold detection
- `lib/protocol` — BLE packet encoding/validation
- `src/app` — event queue and the controller thread
- `src/drivers` — BLE GATT transport
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
