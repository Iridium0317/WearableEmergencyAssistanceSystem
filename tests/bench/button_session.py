#!/usr/bin/env python3
"""Physical-button session: record while a person presses the real buttons,
then check the log against the expected script and report timing.

    python tests/bench/button_session.py record --seconds 240
    python tests/bench/button_session.py analyze tests/bench/results/<run>_buttons_serial.log

While recording, the script is also the BLE host: it subscribes and auto-acks
every alert, so a held button exercises the full path to the host.

Expected human script (see TEST_RESULTS.md T6):
  1-3. For each button 1, 2, 3: hold ~2 s, release, then quickly tap any
       button once to cancel during PENDING.
  4.   Tap button 1 five times quickly (each tap < 0.5 s): must not alert.
  5.   Hold button 2 ~2 s, release, do not touch: alert goes out over BLE and
       is acknowledged by this script.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import datetime as dt
import statistics
import sys
import time
from pathlib import Path

from bench import BenchFailure, Device, Line, find_port, parse_line
from ble_bench import MSG_ALERT, Host, ack_packet, parse_alert

RESULTS = Path(__file__).parent / "results"
ALERTS = {"1": "EMERGENCY", "2": "MEDICAL", "3": "FAMILY"}


# --- recording ---------------------------------------------------------------


async def record(port: str, seconds: float, stop_file: Path) -> Path:
    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    RESULTS.mkdir(parents=True, exist_ok=True)
    raw_path = RESULTS / f"{run_id}_buttons_serial.log"
    with raw_path.open("w", buffering=1) as raw_log:
        dev = Device(port, raw_log)
        dev.open()
        await asyncio.to_thread(dev.reboot)
        host = Host()
        await host.connect()
        print(f"RECORDING {raw_path}", flush=True)

        async def auto_ack() -> None:
            while True:
                _, data = await host.alerts.get()
                alert = parse_alert(data)
                raw_log.write(f"{time.monotonic():.6f}\t### host rx {alert}\n")
                if alert["msg"] == MSG_ALERT:
                    await host.write_ack(ack_packet(alert["id"]))
                    raw_log.write(f"{time.monotonic():.6f}\t### host acked id={alert['id']}\n")

        acker = asyncio.create_task(auto_ack())
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not stop_file.exists():
            await asyncio.sleep(0.2)
        acker.cancel()
        await host.disconnect()
        dev.close()
    stop_file.unlink(missing_ok=True)
    print(f"STOPPED {raw_path}", flush=True)
    return raw_path


# --- analysis ----------------------------------------------------------------


def load(path: Path) -> tuple[list[Line], list[str]]:
    lines, malformed = [], []
    for row in path.read_text().splitlines():
        t, _, raw = row.partition("\t")
        if not raw or raw.startswith((">>>", "###")):
            continue
        line = parse_line(raw, float(t))
        (lines if line else malformed).append(line or raw)
    return lines, malformed


def per_press_edges(lines: list[Line]) -> list[int]:
    """BTN logs carry cumulative edge counts; per-transition bounce is the delta."""
    last: dict[str, int] = {}
    deltas = []
    for l in lines:
        if l.tag == "BTN" and l.fields.get("ev") in ("PRESSED", "RELEASED"):
            b, edges = l.fields["btn"], int(l.fields["edges"])
            deltas.append(edges - last.get(b, 0))
            last[b] = edges
    return deltas


def check(lines: list[Line], malformed: list[str]) -> list[tuple[str, bool, str]]:
    results = []

    def add(name: str, ok: bool, detail: str = "") -> None:
        results.append((name, ok, detail))

    sm = [l for l in lines if l.tag == "SM" and "to" in l.fields]
    held = [l for l in lines if l.tag == "BTN" and l.fields.get("ev") == "HELD"]
    pressed = [l for l in lines if l.tag == "BTN" and l.fields.get("ev") == "PRESSED"]

    # Every PENDING must come from a real hold: no phantom alerts.
    pendings = [l for l in sm if l.fields["to"] == "PENDING"]
    add("no_phantom_alerts", all(p.fields["ev"] == "BUTTON_HELD" for p in pendings) and len(pendings) == len(held),
        f"pending={len(pendings)} held={len(held)}")

    # Steps 1-3: each button held -> PENDING with the right type -> cancelled.
    for b, alert in ALERTS.items():
        ok = False
        for i, p in enumerate(sm):
            if p.fields["to"] == "PENDING" and p.fields["alert"] == alert:
                nxt = sm[i + 1] if i + 1 < len(sm) else None
                if nxt and nxt.fields["to"] == "CANCELLED" and nxt.fields.get("reason") == "user_cancel":
                    ok = True
                    break
        add(f"button{b}_hold_then_cancel", ok, alert)

    # Step 4: at least 5 presses of button 1 that never reached HELD.
    taps = 0
    for p in pressed:
        if p.fields["btn"] != "1":
            continue
        later_held = [h for h in held if h.fields["btn"] == "1" and h.dev_ms > p.dev_ms]
        nxt_release = [l for l in lines if l.tag == "BTN" and l.fields.get("ev") == "RELEASED"
                       and l.fields["btn"] == "1" and l.dev_ms > p.dev_ms]
        if nxt_release and (not later_held or later_held[0].dev_ms > nxt_release[0].dev_ms):
            taps += 1
    add("short_taps_do_not_alert", taps >= 5, f"short taps on button 1: {taps}")

    # Step 5: a physical hold delivered over BLE and acknowledged by the host.
    delivered = any(l.fields["to"] == "ACKNOWLEDGED" for l in sm) and any(
        l.tag == "BLE_RX" and l.fields.get("msg") == "ACK" for l in lines)
    add("physical_hold_acknowledged_over_ble", delivered)

    errors = [l.raw for l in lines if l.tag in ("BTN_ERR", "BLE_ERR", "DIAG_ERR", "ERR")]
    add("no_error_lines", not errors, "; ".join(errors[:3]))
    add("no_malformed_lines", not malformed, "; ".join(malformed[:3]))
    return results


def metrics(lines: list[Line]) -> list[tuple[str, list[float]]]:
    held_late = [float(l.fields["held_late_ms"]) for l in lines if l.tag == "BTN" and l.fields.get("ev") == "HELD"]
    debounce = [float(l.fields["debounce_ms"]) for l in lines if l.tag == "BTN" and l.fields.get("ev") == "PRESSED"]
    isr = [float(l.fields["isr_to_task_max_us"]) for l in lines
           if l.tag == "BTN" and "isr_to_task_max_us" in l.fields]
    queue = [float(l.fields["lat_ms"]) for l in lines
             if l.tag == "SM" and l.fields.get("ev") in ("BUTTON_HELD", "BUTTON_PRESSED")]
    return [
        ("press_detect_after_last_edge_ms (debounce)", debounce),
        ("held_late_ms (vs exact 1500 ms threshold)", held_late),
        ("isr_to_task_max_us (per transition)", isr),
        ("button_event_queue_latency_ms", queue),
        ("raw_edges_per_transition (bounce)", [float(x) for x in per_press_edges(lines)]),
    ]


def analyze(path: Path) -> bool:
    lines, malformed = load(path)
    results = check(lines, malformed)
    stats = metrics(lines)
    print(f"session: {path}\n")
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name:<38} {detail}")
    print("\nmetric                                          n     min  median     p95     max")
    for name, values in stats:
        if not values:
            print(f"{name:<44} {0:4d}")
            continue
        v = sorted(values)
        p95 = v[min(len(v) - 1, round(0.95 * (len(v) - 1)))]
        print(f"{name:<44} {len(v):4d} {v[0]:7.1f} {statistics.median(v):7.1f} {p95:7.1f} {v[-1]:7.1f}")

    out = path.with_name(path.name.replace("_serial.log", "_analysis.csv"))
    with out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "name", "result_or_n", "detail_or_values"])
        for name, ok, detail in results:
            w.writerow(["check", name, "PASS" if ok else "FAIL", detail])
        for name, values in stats:
            w.writerow(["metric", name, len(values), " ".join(f"{x:g}" for x in values)])
    passed = all(ok for _, ok, _ in results)
    print(f"\n{'PASS' if passed else 'FAIL'}: {sum(ok for _, ok, _ in results)}/{len(results)} checks\nanalysis: {out}")
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    rec = sub.add_parser("record")
    rec.add_argument("--port")
    rec.add_argument("--seconds", type=float, default=240)
    rec.add_argument("--stop-file", type=Path, default=RESULTS / "STOP")
    ana = sub.add_parser("analyze")
    ana.add_argument("log", type=Path)
    args = parser.parse_args()
    if args.cmd == "record":
        try:
            path = asyncio.run(record(args.port or find_port(), args.seconds, args.stop_file))
        except BenchFailure as exc:
            print(f"FAIL: {exc}")
            return 1
        return 0 if analyze(path) else 1
    return 0 if analyze(args.log) else 1


if __name__ == "__main__":
    sys.exit(main())
