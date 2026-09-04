#!/usr/bin/env python3
"""Small HUB/2 smoke for the direct native gateway."""

from __future__ import annotations

import argparse
import os
import selectors
import subprocess
import threading
import time


def read_until(process: subprocess.Popen[bytes], selector: selectors.BaseSelector,
               predicate, timeout: float) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        for _key, _events in selector.select(max(0.0, deadline - time.monotonic())):
            raw = process.stdout.readline() if process.stdout else b""
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            lines.append(line)
            if predicate(line):
                return lines
    return lines


def flood_control(process: subprocess.Popen[bytes], stop: threading.Event) -> None:
    assert process.stdin is not None
    fd = process.stdin.fileno()
    os.set_blocking(fd, False)
    command = (
        b'Q2DZ/HUB/2 PUT id=1 json={"alias":"q2.example.invalid",'
        b'"map":"q2dm1","game":"3zb2","ordered":false}\n'
    )
    batch = command * 40
    while not stop.is_set():
        offset = 0
        while offset < len(batch) and not stop.is_set():
            try:
                offset += os.write(fd, batch[offset:])
            except BlockingIOError:
                time.sleep(0.001)
            except BrokenPipeError:
                return
        time.sleep(0.05)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gateway")
    args = parser.parse_args()
    process = subprocess.Popen(
        [args.gateway], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    selector = selectors.DefaultSelector()
    assert process.stdout is not None and process.stdin is not None
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        process.stdin.write(
            b'Q2DZ/HUB/2 ICE generation=1 json=[{"urls":["stun:example.invalid"]}]\n'
            b'Q2DZ/HUB/2 PUT id=1 json={"alias":"q2.example.invalid",'
            b'"map":"q2dm1","game":"3zb2","ordered":false}\n'
        )
        process.stdin.flush()
        lines = read_until(
            process, selector,
            lambda line: line.startswith("Q2DZ/HUB/2 HEARTBEAT") and "targets=1" in line,
            3.0,
        )
        assert any("targets=1" in line for line in lines), lines
        assert not any("FAILED" in line for line in lines), lines

        flood_stop = threading.Event()
        flooder = threading.Thread(target=flood_control, args=(process, flood_stop), daemon=True)
        flooder.start()
        flood_lines = read_until(
            process, selector,
            lambda line: line.startswith("Q2DZ/HUB/2 HEARTBEAT"),
            2.5,
        )
        flood_stop.set()
        flooder.join(timeout=1.0)
        assert not flooder.is_alive(), "control flood writer did not stop"
        os.set_blocking(process.stdin.fileno(), True)
        assert any(line.startswith("Q2DZ/HUB/2 HEARTBEAT") for line in flood_lines), flood_lines

        process.stdin.write(b"Q2DZ/HUB/2 DROP id=1\n")
        process.stdin.flush()
        lines.extend(read_until(
            process, selector,
            lambda line: line.startswith("Q2DZ/HUB/2 HEARTBEAT") and "targets=0" in line,
            2.0,
        ))
        assert any("targets=0" in line for line in lines), lines

        process.stdin.write(b"Q2DZ/HUB/2 STOP\n")
        process.stdin.flush()
        process.stdin.close()
        assert process.wait(timeout=3.0) == 0
        print("direct gateway HUB/2 smoke: PASS")
        return 0
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)


if __name__ == "__main__":
    raise SystemExit(main())
