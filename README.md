# Wearable Emergency Assistance System

Educational prototype of a wearable emergency-alert controller on an Arduino
Nano 33 BLE Rev2 (nRF52840, Cortex-M4, Mbed OS). It is **not** a medical device
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
(3 attempts max, 5 s ack timeout, 2 s backoff) and then end in a sticky ERROR.

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
