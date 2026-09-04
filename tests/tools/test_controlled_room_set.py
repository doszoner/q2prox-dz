#!/usr/bin/env python3
"""Small retained self-check for the deterministic source mode."""

from __future__ import annotations

import importlib.util
import io
import json
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


HARNESS = Path(__file__).with_name("controlled-room-set.py")
OOB = b"\xff\xff\xff\xff"
ICE = b'[{"urls":["stun:example.invalid"]}]\n'


def read_line(stream: object) -> dict[str, object]:
    selector = selectors.DefaultSelector()
    selector.register(stream, selectors.EVENT_READ)  # type: ignore[arg-type]
    try:
        if not selector.select(timeout=5):
            raise AssertionError("harness did not emit an event")
        line = stream.readline()  # type: ignore[union-attr]
    finally:
        selector.close()
    return json.loads(line)


def read_ready(stream: object) -> dict[str, object]:
    ready = read_line(stream)
    assert ready["event"] == "ready"
    return ready


def _write_fake_peer(directory: Path, body: str) -> Path:
    path = directory / "fake-peer.py"
    path.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
    path.chmod(0o755)
    return path


def _run_webrtc_process(peer_binary: Path, count: int = 1) -> subprocess.Popen[str]:
    process = subprocess.Popen(
        [
            sys.executable,
            str(HARNESS),
            "webrtc",
            "--count",
            str(count),
            "--peer-binary",
            str(peer_binary),
            "--ready-timeout",
            "2",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdin is not None
    process.stdin.write(ICE.decode("ascii"))
    process.stdin.close()
    return process


def _parse_events(output: str) -> list[dict[str, object]]:
    return [json.loads(line) for line in output.splitlines() if line]


def _assert_stopped(events: list[dict[str, object]]) -> None:
    stopped = [event for event in events if event.get("event") == "stopped"]
    assert stopped and stopped[-1].get("children_alive") == 0


def _run_sigterm_regression(directory: Path) -> None:
    fake_peer = _write_fake_peer(
        directory,
        "import sys\n"
        "import time\n"
        "sys.stdin.buffer.readline()\n"
        "while True:\n"
        "    time.sleep(60)\n",
    )
    process = _run_webrtc_process(fake_peer, count=2)
    try:
        assert process.stdout is not None
        started = read_line(process.stdout)
        assert started.get("event") == "started"
        process.send_signal(signal.SIGTERM)
        output, error = process.communicate(timeout=5)
        events = _parse_events(json.dumps(started, sort_keys=True, separators=(",", ":")) + "\n" + output)
        assert process.returncode == 0
        assert "Traceback" not in error
        assert not any(event.get("event") == "error" for event in events)
        assert not any(
            event.get("event") == "ready" and event.get("mode") == "webrtc"
            for event in events
        )
        _assert_stopped(events)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def _run_overlong_line_regression(directory: Path) -> None:
    fake_peer = _write_fake_peer(
        directory,
        "import sys\n"
        "import time\n"
        "sys.stdin.buffer.readline()\n"
        "sys.stdout.buffer.write(b'x' * 4097 + b'\\n')\n"
        "sys.stdout.flush()\n"
        "time.sleep(60)\n",
    )
    process = _run_webrtc_process(fake_peer)
    try:
        assert process.stdout is not None
        started = read_line(process.stdout)
        assert started.get("event") == "started"
        output, error = process.communicate(timeout=5)
        events = _parse_events(json.dumps(started, sort_keys=True, separators=(",", ":")) + "\n" + output)
        assert process.returncode == 1
        assert "Traceback" not in error
        assert {event.get("code") for event in events if event.get("event") == "error"} == {
            "child_event_line_too_long"
        }
        _assert_stopped(events)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def _run_queue_overflow_regression() -> None:
    spec = importlib.util.spec_from_file_location("controlled_room_set", HARNESS)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class FakeProcess:
        def __init__(self, output: bytes) -> None:
            self.stdout = io.BytesIO(output)

    class NoopEvidence:
        pass

    alias = "room.q2test.server01"
    event = json.dumps({"event": "ready", "alias": alias, "peer": 1}, separators=(",", ":"))
    output = ((event + "\n") * (module.MAX_CHILD_EVENTS + 1)).encode("ascii")
    harness = module.PeerHarness(1, "/bin/true", module.DEFAULT_SIGNAL, NoopEvidence())
    harness._read_output(alias, FakeProcess(output))
    assert harness.failure_code() == "child_event_queue_overflow"


def main() -> int:
    process = subprocess.Popen(
        [sys.executable, str(HARNESS), "source", "--count", "40", "--duration", "10"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    first_line = ""
    try:
        assert process.stdout is not None
        ready = read_ready(process.stdout)
        first_line = json.dumps(ready, sort_keys=True, separators=(",", ":")) + "\n"
        endpoints = ready["endpoints"]
        assert isinstance(endpoints, list) and len(endpoints) == 40
        assert endpoints[0] == "127.0.0.1:" + endpoints[0].rsplit(":", 1)[1]
        assert urllib.request.urlopen(ready["http_url"], timeout=2).read().decode("ascii").splitlines() == endpoints

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.bind(("127.0.0.1", 0))
            client.settimeout(1)
            source_port = client.getsockname()[1]
            for endpoint in endpoints:
                host, port = endpoint.rsplit(":", 1)
                client.sendto(OOB + b"status\n", (host, int(port)))
                assert client.recvfrom(4096)[0].startswith(OOB + b"print\n")
            host, port = endpoints[-1].rsplit(":", 1)
            client.sendto(OOB + b"getchallenge 7\n", (host, int(port)))
            assert client.recvfrom(1024)[0] == OOB + b"challenge 424242\n"
            client.sendto(OOB + b"connect 424242 \\password\\secret\n", (host, int(port)))

        process.terminate()
        output, error = process.communicate(timeout=3)
        assert process.returncode == 0 and not error
        lines = [json.loads(line) for line in (first_line + output).splitlines() if line]
        requests = [line for line in lines if line.get("event") == "udp_request"]
        assert len([line for line in requests if line["command"] == "status"]) == 40
        assert {line["target"] for line in requests if line["command"] == "status"} == {
            f"server{index:02d}" for index in range(1, 41)
        }
        assert {line["source"] for line in requests} == {f"127.0.0.1:{source_port}"}
        assert [line["command"] for line in requests[-2:]] == ["getchallenge", "connect"]
        assert "secret" not in (first_line + output).lower()
        assert lines[-1] == {"cleanup": "complete", "event": "stopped", "mode": "source"}
        with tempfile.TemporaryDirectory(prefix="q2dz-task7-regression-") as temporary:
            directory = Path(temporary)
            _run_sigterm_regression(directory)
            _run_overlong_line_regression(directory)
        _run_queue_overflow_regression()
        print("controlled-room-set source self-check: PASS")
        return 0
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
