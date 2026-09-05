#!/usr/bin/env python3
"""Small retained self-check for the deterministic source mode."""

from __future__ import annotations

from argparse import Namespace
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
CONTROLLED_PEER = Path(__file__).with_name("q2dz-controlled-peer.cpp")
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


def _run_webrtc_process(
    peer_binary: Path,
    count: int = 1,
    mode: str = "webrtc",
    duration: float | None = None,
    alias_prefix: str | None = None,
) -> subprocess.Popen[str]:
    command = [sys.executable, str(HARNESS), mode]
    if mode == "webrtc":
        command.extend(["--count", str(count)])
    if alias_prefix is not None:
        command.extend(["--alias-prefix", alias_prefix])
    command.extend(["--peer-binary", str(peer_binary), "--ready-timeout", "2"])
    if duration is not None:
        command.extend(["--duration", str(duration)])
    process = subprocess.Popen(
        command,
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


def _run_readiness_timeout_regression(directory: Path) -> None:
    fake_peer = _write_fake_peer(
        directory,
        "import sys\n"
        "import time\n"
        "sys.stdin.buffer.readline()\n"
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
        assert [event for event in events if event.get("event") == "error"] == [
            {
                "code": "peer_readiness_timeout",
                "event": "error",
                "mode": "webrtc",
                "ready_count": 0,
                "target_count": 1,
            }
        ]
        _assert_stopped(events)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def _run_stopped_timeout_regression() -> None:
    spec = importlib.util.spec_from_file_location("controlled_room_set_stopped_timeout", HARNESS)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class FakeEvidence:
        def __init__(self) -> None:
            self.events: list[dict[str, object]] = []

        def emit(self, event: dict[str, object]) -> None:
            self.events.append(event)

    class FakeHarness:
        last: "FakeHarness | None" = None

        def __init__(
            self,
            _count: int,
            _binary: str,
            _signal: str,
            evidence: FakeEvidence,
            _alias_prefix: str = "room.q2test.server",
        ) -> None:
            self.ready: dict[str, int] = {}
            self.count = 1
            self.evidence = evidence
            self.closed = False
            FakeHarness.last = self

        def start(self, _ice: bytes) -> None:
            return

        def wait_ready(self, _timeout: float) -> bool:
            raise TimeoutError("synthetic readiness timeout")

        def close(self) -> None:
            self.closed = True
            self.evidence.emit({"event": "stopped", "children_alive": 0})

    evidence = FakeEvidence()
    module.PeerHarness = FakeHarness
    module._read_ice_stdin = lambda: b"[]"
    module._STOP.set()
    try:
        result = module.webrtc_main(
            Namespace(
                count=1,
                peer_binary="/bin/true",
                signal=module.DEFAULT_SIGNAL,
                ready_timeout=1.0,
                duration=None,
            ),
            evidence,
        )
        assert result == 0
        assert FakeHarness.last is not None and FakeHarness.last.closed
        assert evidence.events == [{"event": "stopped", "children_alive": 0}]
    finally:
        module._STOP.clear()


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

    alias = "room.q2ded.testserver01"
    event = json.dumps({"event": "ready", "alias": alias, "peer": 1}, separators=(",", ":"))
    output = ((event + "\n") * (module.MAX_CHILD_EVENTS + 1)).encode("ascii")
    harness = module.PeerHarness(1, "/bin/true", module.DEFAULT_SIGNAL, NoopEvidence())
    harness._read_output(alias, FakeProcess(output))
    assert harness.failure_code() == "child_event_queue_overflow"


def _run_controlled_peer_readiness_contract_regression() -> None:
    source = CONTROLLED_PEER.read_text(encoding="utf-8")
    assert "humblenet_p2p_is_alias_registered(" in source
    assert "humblenet_p2p_alias_query" not in source
    assert '\\"aliases\\":[' in source
    assert "std::chrono::steady_clock" in source
    assert "alias_registration_timeout" in source
    assert source.count("own_peer = humblenet_p2p_get_my_peer_id();") >= 2


def _run_two_alias_diagnostic_regression() -> None:
    peer_source = CONTROLLED_PEER.read_text(encoding="utf-8")
    assert "--diagnostic-two-alias" in peer_source

    spec = importlib.util.spec_from_file_location("controlled_room_set_two_alias", HARNESS)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class FakeProcess:
        def __init__(self, output: bytes) -> None:
            self.stdout = io.BytesIO(output)

    class NoopEvidence:
        def emit(self, _event: dict[str, object]) -> None:
            return

    first = "room.q2ded.testserver01"
    second = "room.q2ded.testserver02"
    assert module._safe_child_event(
        [first, second],
        json.dumps(
            {"event": "request", "alias": first, "kind": "ping", "peer": 9, "bytes": 8},
            separators=(",", ":"),
        ).encode("ascii"),
    ) is None
    output = "\n".join(
        [
            json.dumps({"event": "ready", "alias": first, "peer": 7}, separators=(",", ":")),
            json.dumps({"event": "ready", "alias": second, "peer": 7}, separators=(",", ":")),
            json.dumps({"event": "request", "aliases": [first, second], "kind": "ping", "peer": 9, "bytes": 8}, separators=(",", ":")),
            json.dumps({"event": "request", "aliases": [first, second], "kind": "status", "peer": 9, "bytes": 9}, separators=(",", ":")),
        ]
    ).encode("ascii") + b"\n"
    diagnostic = module.PeerHarness(
        1,
        "/bin/true",
        module.DEFAULT_SIGNAL,
        NoopEvidence(),
        alias_groups=[[first, second]],
    )
    diagnostic._read_output([first, second], FakeProcess(output))
    diagnostic._apply_events()
    assert diagnostic.ready == {first: 7, second: 7}
    assert diagnostic.group_stats[(first, second)] == {
        "bytes": 17,
        "ping": 1,
        "requests": 2,
        "status": 1,
    }
    assert diagnostic.stats[first]["requests"] == 0
    assert diagnostic.stats[second]["requests"] == 0

    mismatched = module.PeerHarness(
        1,
        "/bin/true",
        module.DEFAULT_SIGNAL,
        NoopEvidence(),
        alias_groups=[[first, second]],
    )
    mismatched._read_output([first, second], FakeProcess(
        "\n".join(
            [
                json.dumps({"event": "ready", "alias": first, "peer": 7}, separators=(",", ":")),
                json.dumps({"event": "ready", "alias": second, "peer": 8}, separators=(",", ":")),
            ]
        ).encode("ascii") + b"\n"
    ))
    try:
        mismatched._apply_events()
    except RuntimeError as error:
        assert str(error) == "controlled peer aliases changed identity"
    else:
        raise AssertionError("diagnostic aliases must share one peer identity")

    ordinary = module.PeerHarness(2, "/bin/true", module.DEFAULT_SIGNAL, NoopEvidence())
    ordinary._read_output(first, FakeProcess(
        (json.dumps({"event": "ready", "alias": first, "peer": 7}, separators=(",", ":")) + "\n").encode("ascii")
    ))
    ordinary._read_output(second, FakeProcess(
        (json.dumps({"event": "ready", "alias": second, "peer": 7}, separators=(",", ":")) + "\n").encode("ascii")
    ))
    try:
        ordinary._apply_events()
    except RuntimeError as error:
        assert str(error) == "duplicate controlled peer id"
    else:
        raise AssertionError("ordinary peers must retain duplicate identity rejection")


def _run_two_alias_diagnostic_process_regression(directory: Path) -> None:
    expected_command = [
        "--diagnostic-two-alias",
        "--alias",
        "room.q2ded.testserver01",
        "--alias",
        "room.q2ded.testserver02",
        "--signal",
        "wss://net.js-dos.com:444",
    ]
    fake_peer = _write_fake_peer(
        directory,
        "import json\n"
        "import sys\n"
        "import time\n"
        f"if sys.argv[1:] != {expected_command!r}: raise SystemExit(3)\n"
        "sys.stdin.buffer.readline()\n"
        "for alias in (\"room.q2ded.testserver01\", \"room.q2ded.testserver02\"):\n"
        "    print(json.dumps({\"event\": \"ready\", \"alias\": alias, \"peer\": 7}, separators=(\",\", \":\")), flush=True)\n"
        "for kind, size in [(\"ping\", 8), (\"status\", 9)]:\n"
        "    print(json.dumps({\"event\": \"request\", \"aliases\": [\"room.q2ded.testserver01\", \"room.q2ded.testserver02\"], \"kind\": kind, \"peer\": 9, \"bytes\": size}, separators=(\",\", \":\")), flush=True)\n"
        "time.sleep(60)\n",
    )
    process = _run_webrtc_process(fake_peer, mode="webrtc-two-alias", duration=0.2)
    try:
        assert process.stdout is not None
        started = read_line(process.stdout)
        assert started["event"] == "started"
        output, error = process.communicate(timeout=5)
        events = _parse_events(json.dumps(started, sort_keys=True, separators=(",", ":")) + "\n" + output)
        assert process.returncode == 0
        assert not error
        assert {event.get("alias") for event in events if event.get("event") == "peer_ready"} == {
            "room.q2ded.testserver01",
            "room.q2ded.testserver02",
        }
        stats = [event for event in events if event.get("event") == "peer_stats"]
        assert stats and "targets" not in stats[-1]
        assert stats and stats[-1]["groups"] == [{
            "aliases": ["room.q2ded.testserver01", "room.q2ded.testserver02"],
            "bytes": 17,
            "ping": 1,
            "requests": 2,
            "status": 1,
        }]
        assert [
            event for event in events
            if event.get("event") == "ready" and event.get("mode") == "webrtc-two-alias"
        ] == [
            {
                "aliases": ["room.q2ded.testserver01", "room.q2ded.testserver02"],
                "event": "ready",
                "mode": "webrtc-two-alias",
                "peer_count": 1,
                "target_count": 2,
            }
        ]
        stopped = [event for event in events if event.get("event") == "stopped"]
        assert stopped and stopped[-1] == {
            "children_alive": 0,
            "cleanup": "complete",
            "event": "stopped",
            "mode": "webrtc-two-alias",
        }
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def _run_alias_prefix_process_regression(directory: Path) -> None:
    expected_command = [
        "--alias",
        "room.q2ded.server01",
        "--signal",
        "wss://net.js-dos.com:444",
    ]
    fake_peer = _write_fake_peer(
        directory,
        "import json\n"
        "import sys\n"
        "import time\n"
        f"if sys.argv[1:] != {expected_command!r}: raise SystemExit(3)\n"
        "sys.stdin.buffer.readline()\n"
        "print(json.dumps({\"event\": \"ready\", \"alias\": \"room.q2ded.server01\", \"peer\": 7}, separators=(\",\", \":\")), flush=True)\n"
        "time.sleep(60)\n",
    )
    process = _run_webrtc_process(
        fake_peer,
        count=1,
        duration=0.2,
        alias_prefix="room.q2ded.server",
    )
    try:
        assert process.stdout is not None
        started = read_line(process.stdout)
        assert started["event"] == "started"
        output, error = process.communicate(timeout=5)
        events = _parse_events(json.dumps(started, sort_keys=True, separators=(",", ":")) + "\n" + output)
        assert process.returncode == 0
        assert not error
        assert [event.get("alias") for event in events if event.get("event") == "peer_ready"] == [
            "room.q2ded.server01"
        ]
        _assert_stopped(events)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def _run_source_check_regression(directory: Path) -> None:
    evidence = directory / "source-events.jsonl"
    evidence.write_text(
        "\n".join(
            [
                json.dumps({"event": "ready", "endpoint_count": 2}, separators=(",", ":")),
                json.dumps(
                    {
                        "event": "udp_request",
                        "target": "server01",
                        "source": "127.0.0.1:50000",
                        "command": "status",
                    },
                    separators=(",", ":"),
                ),
                json.dumps(
                    {
                        "event": "udp_request",
                        "target": "server02",
                        "source": "127.0.0.1:50000",
                        "command": "status",
                    },
                    separators=(",", ":"),
                ),
                json.dumps(
                    {
                        "event": "udp_request",
                        "target": "server02",
                        "source": "127.0.0.1:50000",
                        "command": "getchallenge",
                    },
                    separators=(",", ":"),
                ),
                json.dumps(
                    {
                        "event": "udp_request",
                        "target": "server02",
                        "source": "127.0.0.1:50000",
                        "command": "connect",
                    },
                    separators=(",", ":"),
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    ok = subprocess.run(
        [sys.executable, str(HARNESS), "source-check", "--count", "2", "--evidence", str(evidence)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert ok.returncode == 0
    assert '"result":"PASS"' in ok.stdout

    bad = subprocess.run(
        [sys.executable, str(HARNESS), "source-check", "--count", "3", "--evidence", str(evidence)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert bad.returncode == 1
    assert '"result":"FAIL"' in bad.stdout


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
            _run_readiness_timeout_regression(directory)
            _run_two_alias_diagnostic_process_regression(directory)
            _run_alias_prefix_process_regression(directory)
            _run_source_check_regression(directory)
        _run_stopped_timeout_regression()
        _run_queue_overflow_regression()
        _run_controlled_peer_readiness_contract_regression()
        _run_two_alias_diagnostic_regression()
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
