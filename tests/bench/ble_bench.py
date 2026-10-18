#!/usr/bin/env python3
"""BLE bench tests: the Mac acts as the host over BLE (bleak) while the serial
console injects button events and reports state transitions.

Requires the production firmware (pio run -e nano33ble), not the _sim build.

Usage:
    python tests/bench/ble_bench.py
    python tests/bench/ble_bench.py --only happy_path --repeat 10

Timing caveat: host-side latencies compare the arrival time of a serial log
line with the arrival time of a BLE packet on the same host clock. Both paths
add their own delay (USB CDC, macOS BLE stack, connection interval), so these
numbers are end-to-end host-observed latencies, not on-air timings.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import datetime as dt
import struct
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

from bench import (ACK_TIMEOUT_MS, DELIVERY_TIMEOUT_MS, PENDING_MS, RETRY_BACKOFF_MS, BenchFailure,
                   Device, Metrics,
                   check_window, event_ms, find_port, ms, print_summary)

DEVICE_NAME = "WEA-Nano33"
ALERT_UUID = "8f1d0002-6c2a-4b7e-9e3d-5a4c3b2a1f00"
ACK_UUID = "8f1d0003-6c2a-4b7e-9e3d-5a4c3b2a1f00"
STATUS_UUID = "8f1d0004-6c2a-4b7e-9e3d-5a4c3b2a1f00"

MSG_ALERT, MSG_CANCEL, MSG_ACK = 0x01, 0x02, 0x81
STATE_NAMES = ["IDLE", "PENDING", "CONFIRMED", "ALERT_SENT", "ACKNOWLEDGED", "CANCELLED", "RETRY", "ERROR"]
ALERT_NAMES = {1: "EMERGENCY", 2: "MEDICAL", 3: "FAMILY"}


def ack_packet(alert_id: int) -> bytes:
    return struct.pack("<BBH", 1, MSG_ACK, alert_id)


def parse_alert(data: bytes) -> dict:
    if len(data) != 6:
        raise BenchFailure(f"alert packet length {len(data)} != 6: {data.hex()}")
    version, msg, alert_id, alert_type, attempt = struct.unpack("<BBHBB", data)
    if version != 1 or msg not in (MSG_ALERT, MSG_CANCEL) or alert_type not in ALERT_NAMES:
        raise BenchFailure(f"malformed alert packet: {data.hex()}")
    return {"msg": msg, "id": alert_id, "alert": ALERT_NAMES[alert_type], "attempt": attempt}


def parse_status(data: bytes) -> dict:
    if len(data) != 6 or data[0] != 1 or data[1] >= len(STATE_NAMES):
        raise BenchFailure(f"malformed status packet: {data.hex()}")
    _, state, alert_id, _, attempts = struct.unpack("<BBHBB", data)
    return {"state": STATE_NAMES[state], "id": alert_id, "attempts": attempts}


class Host:
    """BLE central. Notifications are queued with their host arrival time."""

    def __init__(self):
        self.client: BleakClient | None = None
        self.alerts: asyncio.Queue = asyncio.Queue()
        self.statuses: list[tuple[float, dict]] = []

    async def connect(self, subscribe: bool = True, timeout_s: float = 15.0) -> None:
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout_s)
        if device is None:
            raise BenchFailure(f"{DEVICE_NAME} not found while scanning")
        self.client = BleakClient(device)
        await self.client.connect()
        if subscribe:
            await self.client.start_notify(ALERT_UUID, self._on_alert)
            await self.client.start_notify(STATUS_UUID, self._on_status)

    async def disconnect(self) -> None:
        if self.client is not None and self.client.is_connected:
            await self.client.disconnect()
        self.client = None

    def _on_alert(self, _char, data: bytearray) -> None:
        self.alerts.put_nowait((time.monotonic(), bytes(data)))

    def _on_status(self, _char, data: bytearray) -> None:
        self.statuses.append((time.monotonic(), parse_status(bytes(data))))

    async def next_alert(self, timeout_s: float) -> tuple[float, dict]:
        try:
            t, data = await asyncio.wait_for(self.alerts.get(), timeout_s)
        except asyncio.TimeoutError:
            raise BenchFailure("no alert notification received") from None
        return t, parse_alert(data)

    async def write_ack(self, payload: bytes) -> float:
        t = time.monotonic()
        await self.client.write_gatt_char(ACK_UUID, payload, response=True)
        return t


async def expect(dev: Device, tag: str, timeout_s: float, **match: str):
    return await asyncio.to_thread(dev.expect, tag, timeout_s, **match)


# --- scenarios -------------------------------------------------------------


async def scenario_happy_path(dev: Device, host: Host, m: Metrics) -> None:
    await host.connect()
    await expect(dev, "BLE", 5.0, ev="subscribed", char="alert")
    dev.send("hold 2")
    pend = await expect(dev, "SM", 1.0, to="PENDING", alert="MEDICAL")
    m.add("button_event_queue_latency_dev_ms", int(pend.fields["lat_ms"]))
    conf = await expect(dev, "SM", PENDING_MS / 1000 + 1, to="CONFIRMED")
    check_window("pending_window_dev_ms", conf.dev_ms - event_ms(pend), PENDING_MS, m)

    t_notify, alert = await host.next_alert(2.0)
    if alert != {"msg": MSG_ALERT, "id": int(conf.fields["id"]), "alert": "MEDICAL", "attempt": 1}:
        raise BenchFailure(f"unexpected alert packet {alert}")
    m.add("confirmed_log_to_notify_host_ms", ms(conf.host_t, t_notify))
    await expect(dev, "SM", 1.0, to="ALERT_SENT")

    t_ack = await host.write_ack(ack_packet(alert["id"]))
    await expect(dev, "BLE_RX", 1.0, msg="ACK", id=str(alert["id"]))
    ackd = await expect(dev, "SM", 1.0, to="ACKNOWLEDGED")
    m.add("ack_write_to_acknowledged_host_ms", ms(t_ack, ackd.host_t))

    await asyncio.sleep(0.3)  # let the last status notification arrive
    seen = [s["state"] for _, s in host.statuses]
    for state in ("PENDING", "CONFIRMED", "ALERT_SENT", "ACKNOWLEDGED"):
        if state not in seen:
            raise BenchFailure(f"status characteristic never reported {state}: {seen}")


async def scenario_no_host(dev: Device, host: Host, m: Metrics) -> None:
    dev.send("hold 1")
    pend = await expect(dev, "SM", 1.0, to="PENDING")
    await expect(dev, "BLE_TX", PENDING_MS / 1000 + 1, msg="ALERT", ok="0", reason="not_connected")
    retry = await expect(dev, "SM", 1.0, to="RETRY", reason="link_not_ready")
    if retry.fields["attempt"] != "0":
        raise BenchFailure(f"not-ready probe used an attempt: {retry.raw}")
    err = await expect(dev, "SM", DELIVERY_TIMEOUT_MS / 1000 + 1, to="ERROR", reason="delivery_timeout")
    if err.fields["attempt"] != "0":
        raise BenchFailure(f"attempts used with no host: {err.raw}")
    check_window("hold_to_error_no_host_dev_ms", err.dev_ms - event_ms(pend), PENDING_MS + DELIVERY_TIMEOUT_MS, m)


async def scenario_not_subscribed(dev: Device, host: Host, m: Metrics) -> None:
    # Connected but no CCCD subscription: ArduinoBLE would "succeed" silently,
    # so the firmware must detect this itself and treat it as not ready.
    await host.connect(subscribe=False)
    await expect(dev, "BLE", 5.0, ev="connected")
    dev.send("hold 1")
    await expect(dev, "SM", 1.0, to="PENDING")
    await expect(dev, "BLE_TX", PENDING_MS / 1000 + 1, msg="ALERT", ok="0", reason="not_subscribed")
    err = await expect(dev, "SM", DELIVERY_TIMEOUT_MS / 1000 + 1, to="ERROR", reason="delivery_timeout")
    if err.fields["attempt"] != "0":
        raise BenchFailure(f"attempts used while not subscribed: {err.raw}")
    if not host.alerts.empty():
        raise BenchFailure("notification delivered without a subscription")


async def scenario_ack_timeout_then_recover(dev: Device, host: Host, m: Metrics) -> None:
    await host.connect()
    await expect(dev, "BLE", 5.0, ev="subscribed", char="alert")
    dev.send("hold 3")
    _, first = await host.next_alert(PENDING_MS / 1000 + 2)
    sent = await expect(dev, "SM", 1.0, to="ALERT_SENT")
    retry = await expect(dev, "SM", ACK_TIMEOUT_MS / 1000 + 1, to="RETRY", reason="ack_timeout")
    check_window("ack_timeout_dev_ms", retry.dev_ms - event_ms(sent), ACK_TIMEOUT_MS, m)

    t_second, second = await host.next_alert(RETRY_BACKOFF_MS / 1000 + 2)
    if second["id"] != first["id"] or second["attempt"] != 2:
        raise BenchFailure(f"retry packet {second} does not continue {first}")
    await host.write_ack(ack_packet(second["id"]))
    ackd = await expect(dev, "SM", 1.0, to="ACKNOWLEDGED")
    m.add("retry_to_acknowledged_dev_ms", ackd.dev_ms - event_ms(retry))


async def scenario_disconnect_mid_alert(dev: Device, host: Host, m: Metrics) -> None:
    await host.connect()
    await expect(dev, "BLE", 5.0, ev="subscribed", char="alert")
    dev.send("hold 1")
    _, first = await host.next_alert(PENDING_MS / 1000 + 2)
    await expect(dev, "SM", 1.0, to="ALERT_SENT")

    t_disc = time.monotonic()
    await host.disconnect()
    retry = await expect(dev, "SM", 5.0, to="RETRY", reason="link_down")
    m.add("host_disconnect_to_retry_host_ms", ms(t_disc, retry.host_t))

    # Reconnect and re-subscribe (the device clears CCCDs on disconnect). Probes
    # while the host is not connected+subscribed cost no attempt, and the
    # subscription itself triggers the resend, so it must arrive as attempt 2.
    await host.connect()
    t_redelivered, again = await host.next_alert(DELIVERY_TIMEOUT_MS / 1000)
    if again["id"] != first["id"] or again["attempt"] != 2:
        raise BenchFailure(f"unexpected redelivery {again}")
    m.add("disconnect_to_redelivered_host_ms", ms(t_disc, t_redelivered))
    m.add("redelivered_on_attempt", again["attempt"], "n")
    await host.write_ack(ack_packet(again["id"]))
    await expect(dev, "SM", 1.0, to="ACKNOWLEDGED")


async def scenario_malformed_ack(dev: Device, host: Host, m: Metrics) -> None:
    await host.connect()
    await expect(dev, "BLE", 5.0, ev="subscribed", char="alert")
    dev.send("hold 1")
    _, alert = await host.next_alert(PENDING_MS / 1000 + 2)
    await expect(dev, "SM", 1.0, to="ALERT_SENT")
    good = alert["id"]

    mark = dev.mark()
    cases = [
        (bytes([1, MSG_ACK, good & 0xFF]), "bad_length"),
        (ack_packet(good) + b"\x00", "bad_length"),
        (bytes([2, MSG_ACK]) + struct.pack("<H", good), "bad_version"),
        (bytes([1, MSG_ALERT]) + struct.pack("<H", good), "bad_type"),
        (ack_packet(0), "bad_id"),
    ]
    for payload, reason in cases:
        await host.write_ack(payload)
        await expect(dev, "BLE_ERR", 1.0, op="rx_ack", reason=reason)
    # Well-formed but for an alert that isn't active: the state machine rejects it.
    await host.write_ack(ack_packet(good + 50))
    await expect(dev, "SM_REJECT", 1.0, state="ALERT_SENT", reason="ack_id_mismatch")
    if dev.find(mark, None, "SM"):
        raise BenchFailure("a malformed or mismatched ack changed state")

    await host.write_ack(ack_packet(good))
    await expect(dev, "SM", 1.0, to="ACKNOWLEDGED")


async def scenario_cancel_after_send(dev: Device, host: Host, m: Metrics) -> None:
    await host.connect()
    await expect(dev, "BLE", 5.0, ev="subscribed", char="alert")
    dev.send("hold 2")
    _, alert = await host.next_alert(PENDING_MS / 1000 + 2)
    await expect(dev, "SM", 1.0, to="ALERT_SENT")
    t_press = dev.send("press")
    await expect(dev, "SM", 1.0, to="CANCELLED", reason="user_cancel")
    t_cancel, cancel = await host.next_alert(2.0)
    if cancel["msg"] != MSG_CANCEL or cancel["id"] != alert["id"]:
        raise BenchFailure(f"expected CANCEL for id {alert['id']}, got {cancel}")
    m.add("press_to_cancel_notify_host_ms", ms(t_press, t_cancel))


SCENARIOS = {
    "happy_path": scenario_happy_path,
    "no_host": scenario_no_host,
    "not_subscribed": scenario_not_subscribed,
    "ack_timeout_then_recover": scenario_ack_timeout_then_recover,
    "disconnect_mid_alert": scenario_disconnect_mid_alert,
    "malformed_ack": scenario_malformed_ack,
    "cancel_after_send": scenario_cancel_after_send,
}


# --- runner ----------------------------------------------------------------


async def run(port: str, plan: list[str], out_dir: Path) -> bool:
    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = out_dir / f"{run_id}_ble_serial.log"
    results, metrics_rows = [], []

    with raw_path.open("w") as raw_log:
        dev = Device(port, raw_log)
        dev.open()
        try:
            for name in plan:
                host = Host()
                m = Metrics()
                start = time.monotonic()
                malformed_before = len(dev.malformed)
                detail = ""
                try:
                    await asyncio.to_thread(dev.reboot)
                    dev.cursor = dev.mark()
                    await expect(dev, "BLE", 5.0, ev="advertising")
                    await SCENARIOS[name](dev, host, m)
                    if len(dev.malformed) > malformed_before:
                        raise BenchFailure(f"malformed line from device: {dev.malformed[-1]!r}")
                    passed = True
                except BenchFailure as exc:
                    passed, detail = False, str(exc)
                finally:
                    try:
                        await host.disconnect()
                    except Exception:  # noqa: BLE001 - cleanup must not mask the result
                        pass
                duration = time.monotonic() - start
                results.append({"run_id": run_id, "scenario": name, "result": "PASS" if passed else "FAIL",
                                "duration_s": f"{duration:.2f}", "detail": detail})
                metrics_rows += [[run_id, name, metric, value, unit] for metric, value, unit in m.rows]
                print(f"{'PASS' if passed else 'FAIL'}  {name:<26} {duration:6.1f}s  {detail}", flush=True)
        finally:
            dev.close()

    with (out_dir / f"{run_id}_ble_results.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run_id", "scenario", "result", "duration_s", "detail"])
        w.writeheader()
        w.writerows(results)
    with (out_dir / f"{run_id}_ble_metrics.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["run_id", "scenario", "metric", "value", "unit"])
        w.writerows(metrics_rows)
    print_summary(results, metrics_rows)
    print(f"\nartifacts: {out_dir}/{run_id}_ble_*")
    return all(r["result"] == "PASS" for r in results)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: auto-detect Arduino)")
    parser.add_argument("--only", nargs="+", choices=SCENARIOS, help="run only these scenarios")
    parser.add_argument("--repeat", type=int, default=1, help="run each scenario N times")
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "results")
    args = parser.parse_args()
    plan = [name for name in (args.only or SCENARIOS) for _ in range(args.repeat)]
    return 0 if asyncio.run(run(args.port or find_port(), plan, args.out)) else 1


if __name__ == "__main__":
    sys.exit(main())
