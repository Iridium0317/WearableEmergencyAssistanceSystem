#!/usr/bin/env python3
"""Serial bench tests for the wearable emergency firmware.

Drives the firmware's serial console, checks the state transitions it logs,
measures timing, and writes results to CSV. Each scenario reboots the device
first so it starts from IDLE.

Usage:
    python tests/bench/bench.py                 # auto-detect port, run all
    python tests/bench/bench.py --only happy_path cancel_in_pending
    python tests/bench/bench.py --list

Requires pyserial (pip install -r tests/bench/requirements.txt).
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import serial
from serial.tools import list_ports

ARDUINO_VID = 0x2341
LINE_RE = re.compile(r"^(\d+) ([A-Z_]+)(?: (.*))?$")

# Firmware timing constants (kDefaultTiming in lib/app/state_machine.h).
PENDING_MS = 5000  # console holds never release, so the window is timed from HELD
ACK_TIMEOUT_MS = 5000
RETRY_BACKOFF_MS = 2000
MAX_ATTEMPTS = 3
DELIVERY_TIMEOUT_MS = 20000  # first CONFIRMED -> ERROR if unacknowledged
ACK_DISPLAY_MS = 3000
CANCEL_DISPLAY_MS = 1000
# Deadlines are polled every 10 ms on the device; allow that plus slack.
TIMING_TOLERANCE_MS = 30


class BenchFailure(Exception):
    pass


@dataclass
class Line:
    host_t: float  # time.monotonic() when the host received the line
    dev_ms: int  # device uptime stamp from the line itself
    tag: str
    fields: dict[str, str]
    raw: str


def parse_line(raw: str, host_t: float) -> Line | None:
    """Returns None for anything not in '<ms> <TAG> k=v ...' form."""
    m = LINE_RE.match(raw)
    if not m:
        return None
    fields: dict[str, str] = {}
    for token in (m.group(3) or "").split():
        key, sep, value = token.partition("=")
        if not sep or not key:
            return None
        fields[key] = value
    return Line(host_t, int(m.group(1)), m.group(2), fields, raw)


class Device:
    """Serial connection plus a background reader that timestamps every line."""

    def __init__(self, port: str, raw_log):
        self.port = port
        self.raw_log = raw_log
        self.lines: list[Line] = []
        self.malformed: list[str] = []
        self.cursor = 0
        self._cond = threading.Condition()
        self._ser: serial.Serial | None = None
        self._reader: threading.Thread | None = None

    # --- connection ------------------------------------------------------

    def open(self, timeout_s: float = 10.0) -> None:
        deadline = time.monotonic() + timeout_s
        while True:
            try:
                self._ser = serial.Serial(self.port, 115200, timeout=0.1)
                break
            except serial.SerialException:
                if time.monotonic() > deadline:
                    raise BenchFailure(f"could not open {self.port}")
                time.sleep(0.2)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self) -> None:
        ser, self._ser = self._ser, None
        if ser is not None:
            try:
                ser.close()
            except serial.SerialException:
                pass
        if self._reader is not None:
            self._reader.join(timeout=1.0)

    def _read_loop(self) -> None:
        ser = self._ser
        buf = b""
        while ser is not None and ser is self._ser:
            try:
                # read(n) would sit until n bytes or the timeout arrived,
                # adding up to 100 ms to every latency measurement.
                chunk = ser.read(ser.in_waiting or 1)
            except (serial.SerialException, OSError, TypeError):
                return  # port vanished (e.g. reboot); open() will restart us
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw_bytes, buf = buf.split(b"\n", 1)
                self._ingest(raw_bytes.decode(errors="replace").rstrip("\r"))

    def _ingest(self, raw: str) -> None:
        now = time.monotonic()
        self.raw_log.write(f"{now:.6f}\t{raw}\n")
        if not raw:
            return
        line = parse_line(raw, now)
        with self._cond:
            if line is None:
                self.malformed.append(raw)
            else:
                self.lines.append(line)
            self._cond.notify_all()

    def reboot(self) -> None:
        self.send("reboot")
        self.expect("CMD_OK", 2.0, cmd="reboot")
        self.close()
        time.sleep(1.0)  # USB re-enumeration
        self.open()
        # The device is up once it answers; it boots into IDLE by construction.
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            self.send("ping")
            try:
                self.expect("CMD_OK", 0.5, cmd="ping")
                return
            except BenchFailure:
                continue
        raise BenchFailure("device did not answer ping after reboot")

    # --- I/O ---------------------------------------------------------------

    def send(self, cmd: str) -> float:
        if self._ser is None:
            raise BenchFailure("port not open")
        t = time.monotonic()
        self.raw_log.write(f"{t:.6f}\t>>> {cmd}\n")
        self._ser.write((cmd + "\n").encode())
        return t

    def mark(self) -> int:
        with self._cond:
            return len(self.lines)

    def expect(self, tag: str, timeout_s: float, **match: str) -> Line:
        """Waits for the next matching line after the cursor; advances the cursor."""
        deadline = time.monotonic() + timeout_s
        with self._cond:
            while True:
                for i in range(self.cursor, len(self.lines)):
                    if _matches(self.lines[i], tag, match):
                        self.cursor = i + 1
                        return self.lines[i]
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise BenchFailure(f"timeout waiting for {tag} {match}")
                self._cond.wait(remaining)

    def find(self, start: int, end: int | None, tag: str, **match: str) -> list[Line]:
        with self._cond:
            return [l for l in self.lines[start:end] if _matches(l, tag, match)]


def _matches(line: Line, tag: str, match: dict[str, str]) -> bool:
    return line.tag == tag and all(line.fields.get(k) == v for k, v in match.items())


@dataclass
class Metrics:
    rows: list[tuple[str, float, str]] = field(default_factory=list)

    def add(self, name: str, value: float, unit: str = "ms") -> None:
        self.rows.append((name, value, unit))


def check_window(name: str, actual_ms: int, nominal_ms: int, m: Metrics) -> None:
    m.add(name, actual_ms)
    if not nominal_ms <= actual_ms <= nominal_ms + TIMING_TOLERANCE_MS:
        raise BenchFailure(f"{name}={actual_ms} ms, expected {nominal_ms}..{nominal_ms + TIMING_TOLERANCE_MS}")


def event_ms(line: Line) -> int:
    """Device time of the event behind a transition line.

    Firmware deadlines start at the event's timestamp, but the line is stamped
    when it is printed, lat_ms later (queueing + processing). Measuring from
    the print time under-reports timeouts by that amount.
    """
    return line.dev_ms - int(line.fields.get("lat_ms", "0"))


def ms(a: float, b: float) -> float:
    return round((b - a) * 1000.0, 2)


# --- scenarios -------------------------------------------------------------
# Each takes (device, metrics) and raises BenchFailure on any deviation.


def scenario_ping(dev: Device, m: Metrics) -> None:
    for _ in range(20):
        t = dev.send("ping")
        reply = dev.expect("CMD_OK", 1.0, cmd="ping")
        m.add("ping_rtt_host_ms", ms(t, reply.host_t))


def scenario_happy_path(dev: Device, m: Metrics) -> None:
    t_cmd = dev.send("hold 1")
    pend = dev.expect("SM", 1.0, to="PENDING", alert="EMERGENCY")
    m.add("cmd_to_pending_host_ms", ms(t_cmd, pend.host_t))
    conf = dev.expect("SM", PENDING_MS / 1000 + 1, to="CONFIRMED")
    check_window("pending_window_dev_ms", conf.dev_ms - event_ms(pend), PENDING_MS, m)
    dev.expect("LINK", 1.0, tx="ALERT", attempt="1")
    sent = dev.expect("SM", 1.0, to="ALERT_SENT")
    m.add("confirmed_to_alert_sent_dev_ms", sent.dev_ms - event_ms(conf))

    t_ack = dev.send(f"ack {sent.fields['id']}")
    ackd = dev.expect("SM", 1.0, to="ACKNOWLEDGED")
    m.add("ack_cmd_to_acknowledged_host_ms", ms(t_ack, ackd.host_t))
    idle = dev.expect("SM", ACK_DISPLAY_MS / 1000 + 1, to="IDLE")
    check_window("ack_display_dev_ms", idle.dev_ms - event_ms(ackd), ACK_DISPLAY_MS, m)


def scenario_cancel_in_pending(dev: Device, m: Metrics) -> None:
    start = dev.mark()
    dev.send("hold 2")
    pend = dev.expect("SM", 1.0, to="PENDING", alert="MEDICAL")
    time.sleep(1.0)
    t_cancel = dev.send("press")
    canc = dev.expect("SM", 1.0, to="CANCELLED", reason="user_cancel")
    m.add("cancel_cmd_to_cancelled_host_ms", ms(t_cancel, canc.host_t))
    idle = dev.expect("SM", CANCEL_DISPLAY_MS / 1000 + 1, to="IDLE")
    check_window("cancel_display_dev_ms", idle.dev_ms - event_ms(canc), CANCEL_DISPLAY_MS, m)
    time.sleep(PENDING_MS / 1000)  # past where the alert would have gone out
    if dev.find(start, None, "LINK", tx="ALERT"):
        raise BenchFailure("alert transmitted despite cancel during PENDING")
    if pend.fields["id"] != canc.fields["id"]:
        raise BenchFailure("cancel applied to a different alert id")


def scenario_ack_timeout_to_error(dev: Device, m: Metrics) -> None:
    dev.send("hold 3")
    dev.expect("SM", 1.0, to="PENDING", alert="FAMILY")
    sent = dev.expect("SM", PENDING_MS / 1000 + 1, to="ALERT_SENT")
    for attempt in range(1, MAX_ATTEMPTS):
        retry = dev.expect("SM", ACK_TIMEOUT_MS / 1000 + 1, to="RETRY", reason="ack_timeout")
        check_window(f"ack_timeout_dev_ms_attempt{attempt}", retry.dev_ms - event_ms(sent), ACK_TIMEOUT_MS, m)
        conf = dev.expect("SM", RETRY_BACKOFF_MS / 1000 + 1, to="CONFIRMED", attempt=str(attempt + 1))
        check_window(f"retry_backoff_dev_ms_attempt{attempt}", conf.dev_ms - event_ms(retry), RETRY_BACKOFF_MS, m)
        sent = dev.expect("SM", 1.0, to="ALERT_SENT")
    err = dev.expect("SM", ACK_TIMEOUT_MS / 1000 + 1, to="ERROR", reason="ack_timeout")
    m.add("last_send_to_error_dev_ms", err.dev_ms - event_ms(sent))

    # ERROR must be sticky: still no transition after a while.
    mark = dev.mark()
    time.sleep(2.0)
    if dev.find(mark, None, "SM"):
        raise BenchFailure("left ERROR without user action")
    # A new hold must still start a fresh alert from ERROR.
    dev.send("hold 1")
    # "from" is a Python keyword, hence the dict.
    dev.expect("SM", 1.0, **{"from": "ERROR"}, to="PENDING", attempt="0")


def scenario_link_down_send_fails(dev: Device, m: Metrics) -> None:
    # Link down = host not ready: probes send nothing and use no attempt, and
    # the delivery budget ends the alert.
    dev.send("link down")
    dev.expect("CMD_OK", 1.0, cmd="link")
    dev.send("hold 1")
    pend = dev.expect("SM", 1.0, to="PENDING")
    conf = dev.expect("SM", PENDING_MS / 1000 + 1, to="CONFIRMED")
    retry = dev.expect("SM", 1.0, to="RETRY", reason="link_not_ready")
    if retry.fields["attempt"] != "0":
        raise BenchFailure(f"not-ready probe used an attempt: {retry.raw}")
    err = dev.expect("SM", DELIVERY_TIMEOUT_MS / 1000 + 1, to="ERROR", reason="delivery_timeout")
    if err.fields["attempt"] != "0":
        raise BenchFailure(f"attempts used with the link down: {err.raw}")
    check_window("hold_to_error_link_down_dev_ms", err.dev_ms - event_ms(pend), PENDING_MS + DELIVERY_TIMEOUT_MS, m)
    check_window("first_confirmed_to_error_dev_ms", err.dev_ms - event_ms(conf), DELIVERY_TIMEOUT_MS, m)


def scenario_link_drop_then_recover(dev: Device, m: Metrics) -> None:
    dev.send("hold 1")
    dev.expect("SM", PENDING_MS / 1000 + 1, to="ALERT_SENT")
    t_down = dev.send("link down")
    retry = dev.expect("SM", 1.0, to="RETRY", reason="link_down")
    m.add("link_down_cmd_to_retry_host_ms", ms(t_down, retry.host_t))
    dev.send("link up")
    dev.expect("CMD_OK", 1.0, cmd="link", up="1")
    resent = dev.expect("SM", RETRY_BACKOFF_MS / 1000 + 1, to="ALERT_SENT", attempt="2")
    m.add("link_drop_to_resent_dev_ms", resent.dev_ms - event_ms(retry))
    dev.send(f"ack {resent.fields['id']}")
    dev.expect("SM", 1.0, to="ACKNOWLEDGED")


def scenario_wrong_ack_rejected(dev: Device, m: Metrics) -> None:
    dev.send("ack 1")
    dev.expect("SM_REJECT", 1.0, state="IDLE", reason="ack_without_alert")
    dev.send("hold 2")
    sent = dev.expect("SM", PENDING_MS / 1000 + 1, to="ALERT_SENT")
    good_id = int(sent.fields["id"])
    mark = dev.mark()
    dev.send(f"ack {good_id + 100}")
    dev.expect("SM_REJECT", 1.0, state="ALERT_SENT", reason="ack_id_mismatch")
    if dev.find(mark, None, "SM"):
        raise BenchFailure("state changed on a mismatched ack")
    dev.send(f"ack {good_id}")
    dev.expect("SM", 1.0, to="ACKNOWLEDGED")


def scenario_malformed_commands(dev: Device, m: Metrics) -> None:
    cases = [
        ("hold 9", "bad_button"),
        ("hold x", "bad_button"),
        ("hold", "bad_button"),
        ("ack abc", "bad_id"),
        ("ack 0", "bad_id"),
        ("frobnicate", "unknown_or_bad_args"),
        ("link sideways", "unknown_or_bad_args"),
        ("hold 1 2", "too_many_args"),
        ("x" * 100, "line_too_long"),
    ]
    mark = dev.mark()
    for cmd, reason in cases:
        dev.send(cmd)
        dev.expect("CMD_ERR", 1.0, reason=reason)
    if dev.find(mark, None, "SM"):
        raise BenchFailure("malformed command caused a state transition")
    # Console must still work afterwards.
    dev.send("ping")
    dev.expect("CMD_OK", 1.0, cmd="ping")


SCENARIOS = {
    "ping": scenario_ping,
    "happy_path": scenario_happy_path,
    "cancel_in_pending": scenario_cancel_in_pending,
    "ack_timeout_to_error": scenario_ack_timeout_to_error,
    "link_down_send_fails": scenario_link_down_send_fails,
    "link_drop_then_recover": scenario_link_drop_then_recover,
    "wrong_ack_rejected": scenario_wrong_ack_rejected,
    "malformed_commands": scenario_malformed_commands,
}


# --- runner ----------------------------------------------------------------


def find_port() -> str:
    for p in list_ports.comports():
        if p.vid == ARDUINO_VID:
            return p.device
    raise SystemExit("no Arduino (VID 0x2341) serial port found; use --port")


def run(port: str, names: list[str], out_dir: Path) -> bool:
    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_path = out_dir / f"{run_id}_serial.log"
    results_path = out_dir / f"{run_id}_results.csv"
    metrics_path = out_dir / f"{run_id}_metrics.csv"

    results: list[dict] = []
    metrics_rows: list[list] = []
    with raw_path.open("w") as raw_log:
        dev = Device(port, raw_log)
        dev.open()
        try:
            for name in names:
                m = Metrics()
                start = time.monotonic()
                malformed_before = len(dev.malformed)
                detail = ""
                try:
                    dev.reboot()
                    dev.cursor = dev.mark()
                    scenario_start = dev.mark()
                    SCENARIOS[name](dev, m)
                    errors = dev.find(scenario_start, None, "DIAG_ERR")
                    if errors:
                        raise BenchFailure(f"device reported {errors[0].raw}")
                    if len(dev.malformed) > malformed_before:
                        raise BenchFailure(f"malformed line from device: {dev.malformed[-1]!r}")
                    passed = True
                except BenchFailure as exc:
                    passed = False
                    detail = str(exc)
                duration = time.monotonic() - start
                results.append({"run_id": run_id, "scenario": name, "result": "PASS" if passed else "FAIL",
                                "duration_s": f"{duration:.2f}", "detail": detail})
                for metric, value, unit in m.rows:
                    metrics_rows.append([run_id, name, metric, value, unit])
                print(f"{'PASS' if passed else 'FAIL'}  {name:<26} {duration:6.1f}s  {detail}", flush=True)
        finally:
            dev.close()

    with results_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run_id", "scenario", "result", "duration_s", "detail"])
        w.writeheader()
        w.writerows(results)
    with metrics_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["run_id", "scenario", "metric", "value", "unit"])
        w.writerows(metrics_rows)

    print_summary(results, metrics_rows)
    print(f"\nresults: {results_path}\nmetrics: {metrics_path}\nserial log: {raw_path}")
    return all(r["result"] == "PASS" for r in results)


def print_summary(results: list[dict], metrics_rows: list[list]) -> None:
    by_metric: dict[str, list[float]] = {}
    for _, _, metric, value, _ in metrics_rows:
        by_metric.setdefault(metric, []).append(float(value))
    if by_metric:
        print("\nmetric                                   n      min      avg      max")
        for metric, values in by_metric.items():
            print(f"{metric:<38} {len(values):3d} {min(values):8.2f} {sum(values) / len(values):8.2f} {max(values):8.2f}")
    passed = sum(r["result"] == "PASS" for r in results)
    verdict = "PASS" if passed == len(results) else "FAIL"
    print(f"\n{verdict}: {passed}/{len(results)} scenarios passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: auto-detect Arduino)")
    parser.add_argument("--only", nargs="+", choices=SCENARIOS, help="run only these scenarios")
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "results")
    parser.add_argument("--list", action="store_true", help="list scenarios and exit")
    args = parser.parse_args()
    if args.list:
        print("\n".join(SCENARIOS))
        return 0
    port = args.port or find_port()
    return 0 if run(port, args.only or list(SCENARIOS), args.out) else 1


if __name__ == "__main__":
    sys.exit(main())
