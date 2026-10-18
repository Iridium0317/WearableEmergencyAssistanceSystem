#!/usr/bin/env python3
"""Live wiring check: poll `btn` every 250 ms and print pin-level changes and
button/state-machine log lines. 1 = released, 0 = shorted to GND.
Survives a board reset: when USB drops it waits for the port and reconnects.

    python tests/bench/button_watch.py [seconds]   (Ctrl+C to stop)
"""

from __future__ import annotations

import sys
import time

import serial

from bench import find_port


def reconnect(t0: float, deadline: float) -> serial.Serial | None:
    """Wait for the board to re-enumerate after a reset. Boot lines are kept:
    the firmware waits up to 3 s for the host before logging them."""
    while time.time() < deadline:
        time.sleep(0.2)
        try:
            port = serial.Serial(find_port(), 115200, timeout=0.05)
        except (SystemExit, serial.SerialException):
            continue
        print(f"{time.time() - t0:6.1f}s  --- reconnected to {port.port}", flush=True)
        return port
    return None


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 600
    port = serial.Serial(find_port(), 115200, timeout=0.05)
    time.sleep(0.3)
    port.reset_input_buffer()
    print(f"watching {port.port} for {seconds:.0f} s (Ctrl+C to stop)", flush=True)
    last_levels, buf, t0 = None, b"", time.time()
    try:
        while time.time() - t0 < seconds:
            try:
                port.write(b"btn\n")
                time.sleep(0.25)
                buf += port.read(4000)
            except (serial.SerialException, OSError):
                print(f"{time.time() - t0:6.1f}s  --- device disconnected (reset?), waiting", flush=True)
                port.close()
                port = reconnect(t0, t0 + seconds)
                if port is None:
                    break
                buf, last_levels = b"", None
                continue
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode(errors="replace").strip()
                stamp = f"{time.time() - t0:6.1f}s"
                if "BTN_DIAG" in line:
                    fields = dict(w.split("=", 1) for w in line.split() if "=" in w)
                    levels = " ".join(f"{k}={fields.get(k, '?')}" for k in ("d2", "d3", "d4"))
                    if levels != last_levels:
                        print(f"{stamp}  {levels}  isr_edges={fields.get('isr_edges', '?')}", flush=True)
                        last_levels = levels
                elif any(k in line for k in (" BOOT ", " BTN", " SM ", " SM_REJECT ", " SM_INFO ", "ERR", " BLE ", " ACK ")):
                    print(f"{stamp}  {line}", flush=True)
    except KeyboardInterrupt:
        pass
    print("stopped", flush=True)


if __name__ == "__main__":
    main()
