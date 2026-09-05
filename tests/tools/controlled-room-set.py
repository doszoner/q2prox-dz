#!/usr/bin/env python3
"""Retained 40-target harness for the exact Q2PRO-X and shared gateway gates.

``source`` serves a deterministic text Custom source and one UDP responder per
row.  ``webrtc`` starts controlled WebRTC-NET peers, each of which receives its
ICE configuration through stdin and answers only Quake II OOB ping/status.

The harness never writes the stdin payload to evidence.  Its evidence format
contains only loopback endpoints, neutral aliases, counters, and process
lifecycle facts.
"""

from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import os
import queue
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, TextIO


OOB = b"\xff\xff\xff\xff"
MAX_TARGETS = 40
MAX_ICE_BYTES = 64 * 1024
MAX_CHILD_LINE = 4096
MAX_CHILD_EVENTS = 256
DEFAULT_SIGNAL = "wss://net.js-dos.com:444"
FIXED_CHALLENGE = "424242"


def aliases(count: int, prefix: str = "room.q2test.server") -> list[str]:
    return [f"{prefix}{index:02d}" for index in range(1, count + 1)]


def _safe_alias_prefix(value: str) -> str:
    if not value or len(value) > 96 or not all(char.isalnum() or char in ".-_" for char in value):
        raise argparse.ArgumentTypeError("alias prefix must be a safe token prefix")
    return value


def _safe_count(value: str) -> int:
    count = int(value)
    if not 1 <= count <= MAX_TARGETS:
        raise argparse.ArgumentTypeError(f"count must be in [1,{MAX_TARGETS}]")
    return count


def _safe_duration(value: str) -> float | None:
    duration = float(value)
    if duration <= 0:
        raise argparse.ArgumentTypeError("duration must be positive")
    return duration


class Evidence:
    """Emit only the public, machine-readable part of a run."""

    def __init__(self, path: str | None) -> None:
        self._lock = threading.Lock()
        self._file: TextIO | None = None
        if path and path != "-":
            self._file = Path(path).open("w", encoding="utf-8")

    def emit(self, event: dict[str, Any]) -> None:
        line = json.dumps(event, sort_keys=True, separators=(",", ":"))
        with self._lock:
            print(line, flush=True)
            if self._file is not None:
                self._file.write(line + "\n")
                self._file.flush()

    def close(self) -> None:
        with self._lock:
            if self._file is not None:
                self._file.close()
                self._file = None


def _oob_command(packet: bytes) -> str | None:
    if len(packet) < len(OOB) or packet[:4] != OOB:
        return None
    command = packet[4:].split(b"\n", 1)[0].rstrip(b"\r\0")
    try:
        return command.decode("ascii")
    except UnicodeDecodeError:
        return "invalid"


def _oob_kind(command: str | None) -> str:
    if command is None:
        return "binary"
    if command in {"ping", "status", "getinfo"}:
        return command
    if command == "getchallenge" or command.startswith("getchallenge "):
        return "getchallenge"
    if command == "connect" or command.startswith("connect "):
        return "connect"
    return "other"


def _status_response(server: str) -> bytes:
    info = (
        f"\\hostname\\{server}\\game\\3zb2\\gamedir\\3zb2"
        f"\\mapname\\q2dm1\\levelname\\q2dm1\\protocol\\35"
        f"\\maxclients\\16\\sv_maxclients\\16\\clients\\1\n"
        '0 50 "Player1"\n'
    )
    return OOB + b"print\n" + info.encode("ascii")


class SourceHarness:
    def __init__(self, count: int, evidence: Evidence) -> None:
        self.count = count
        self.evidence = evidence
        self.stop = threading.Event()
        self._closed = False
        self._lock = threading.Lock()
        self.sockets: list[socket.socket] = []
        self.threads: list[threading.Thread] = []
        self.http: http.server.ThreadingHTTPServer | None = None
        self.body = b""
        self.endpoints: list[str] = []
        self.http_url = ""

    def _record_udp(self, index: int, packet: bytes, peer: tuple[str, int]) -> None:
        command = _oob_command(packet)
        self.evidence.emit(
            {
                "event": "udp_request",
                "target": f"server{index + 1:02d}",
                "local": self.endpoints[index],
                "source": f"127.0.0.1:{peer[1]}",
                "command": _oob_kind(command),
                "bytes": len(packet),
            }
        )

    def _udp_loop(self, index: int, sock: socket.socket) -> None:
        server = f"server{index + 1:02d}"
        status = _status_response(server)
        while not self.stop.is_set():
            try:
                packet, peer = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            self._record_udp(index, packet, peer)
            command = _oob_command(packet)
            kind = _oob_kind(command)
            if kind == "ping":
                response = OOB + b"ack\n"
                kind = "ack"
            elif kind == "getchallenge":
                response = OOB + f"challenge {FIXED_CHALLENGE}\n".encode("ascii")
                kind = "challenge"
            elif kind in {"status", "getinfo"}:
                response = status
                kind = "status"
            else:
                continue
            try:
                sent = sock.sendto(response, peer)
            except OSError:
                return
            self.evidence.emit(
                {
                    "event": "udp_response",
                    "target": server,
                    "local": self.endpoints[index],
                    "destination": f"127.0.0.1:{peer[1]}",
                    "kind": kind,
                    "bytes": sent,
                }
            )

    def start(self) -> None:
        for _index in range(self.count):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            sock.settimeout(0.2)
            self.sockets.append(sock)
        self.endpoints = [f"127.0.0.1:{sock.getsockname()[1]}" for sock in self.sockets]
        self.body = ("\n".join(self.endpoints) + "\n").encode("ascii")

        harness = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def do_GET(self) -> None:  # noqa: N802
                if self.path != "/servers":
                    self.send_error(404)
                    return
                body = harness.body
                harness.evidence.emit(
                    {
                        "event": "http_get",
                        "path": self.path,
                        "bytes": len(body),
                        "endpoint_count": harness.count,
                        "first": harness.endpoints[0],
                        "last": harness.endpoints[-1],
                        "body_sha256": hashlib.sha256(body).hexdigest(),
                    }
                )
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=ascii")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.http.daemon_threads = True
        self.http_url = f"http://127.0.0.1:{self.http.server_port}/servers"
        for index, sock in enumerate(self.sockets):
            thread = threading.Thread(target=self._udp_loop, args=(index, sock), daemon=True)
            thread.start()
            self.threads.append(thread)
        http_thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        http_thread.start()
        self.threads.append(http_thread)
        self.evidence.emit(
            {
                "event": "ready",
                "mode": "source",
                "http_url": self.http_url,
                "endpoint_count": self.count,
                "endpoints": self.endpoints,
                "body_sha256": hashlib.sha256(self.body).hexdigest(),
            }
        )

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
        self.stop.set()
        if self.http is not None:
            self.http.shutdown()
            self.http.server_close()
        for sock in self.sockets:
            try:
                sock.close()
            except OSError:
                pass
        for thread in self.threads:
            thread.join(timeout=1.0)
        self.evidence.emit({"event": "stopped", "mode": "source", "cleanup": "complete"})


def _read_ice_stdin() -> bytes:
    data = sys.stdin.buffer.read(MAX_ICE_BYTES + 1)
    if data.endswith(b"\n"):
        data = data[:-1]
        if data.endswith(b"\r"):
            data = data[:-1]
    if not data or len(data) > MAX_ICE_BYTES:
        raise ValueError("ICE input is empty or too large")
    if b"\0" in data or b"\r" in data or b"\n" in data:
        raise ValueError("ICE input must be one line")
    try:
        payload = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("ICE input is not valid JSON") from exc
    if not isinstance(payload, list) or not payload:
        raise ValueError("ICE input must be a non-empty JSON array")
    return data


def _safe_child_event(expected_aliases: str | list[str], raw: bytes) -> dict[str, Any] | None:
    if len(raw) > MAX_CHILD_LINE:
        return None
    try:
        event = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    aliases = {expected_aliases} if isinstance(expected_aliases, str) else set(expected_aliases)
    if not isinstance(event, dict):
        return None
    kind = event.get("event")
    if kind == "ready":
        if event.get("alias") not in aliases:
            return None
        peer = event.get("peer")
        return event if isinstance(peer, int) and peer > 0 else None
    if kind == "request":
        if (isinstance(expected_aliases, str) and event.get("alias") != expected_aliases) or (
            not isinstance(expected_aliases, str) and event.get("aliases") != expected_aliases
        ):
            return None
        if event.get("kind") not in {"ping", "status"}:
            return None
        peer = event.get("peer")
        size = event.get("bytes")
        return event if isinstance(peer, int) and peer > 0 and isinstance(size, int) and size >= 0 else None
    if kind == "stopped":
        if event.get("alias") not in aliases:
            return None
        return event
    if kind == "fatal" and isinstance(event.get("code"), str):
        if event.get("alias") not in aliases:
            return None
        return event
    return None


class PeerHarness:
    def __init__(
        self,
        count: int,
        binary: str,
        signal_url: str,
        evidence: Evidence,
        alias_prefix: str = "room.q2test.server",
        alias_groups: list[list[str]] | None = None,
    ) -> None:
        self.count = count
        self.binary = str(Path(binary).resolve())
        self.signal_url = signal_url
        self.evidence = evidence
        self.processes: list[subprocess.Popen[bytes]] = []
        generated_aliases = aliases(count, alias_prefix)
        self.alias_groups = alias_groups or [[alias] for alias in generated_aliases]
        self.aliases = [alias for group in self.alias_groups for alias in group]
        if not self.aliases or any(not group for group in self.alias_groups):
            raise ValueError("controlled peer alias groups must not be empty")
        if len(set(self.aliases)) != len(self.aliases):
            raise ValueError("controlled peer aliases must be unique")
        self.alias_group = {
            alias: index
            for index, group in enumerate(self.alias_groups)
            for alias in group
        }
        self.mode = "webrtc-two-alias" if any(len(group) > 1 for group in self.alias_groups) else "webrtc"
        self.group: int | None = None
        self.events: queue.Queue[tuple[str | tuple[str, ...], dict[str, Any]]] = queue.Queue(maxsize=MAX_CHILD_EVENTS)
        self._failure_lock = threading.Lock()
        self._failure_code: str | None = None
        self.readers: list[threading.Thread] = []
        self.stats = {
            alias: {"ping": 0, "status": 0, "requests": 0, "bytes": 0}
            for alias in self.aliases
        }
        self.group_stats = {
            tuple(group): {"ping": 0, "status": 0, "requests": 0, "bytes": 0}
            for group in self.alias_groups
        }
        self.ready: dict[str, int] = {}

    def _read_output(self, expected_aliases: str | list[str], process: subprocess.Popen[bytes]) -> None:
        assert process.stdout is not None
        while True:
            raw = process.stdout.readline(MAX_CHILD_LINE + 1)
            if not raw:
                return
            if len(raw) > MAX_CHILD_LINE or not raw.endswith(b"\n"):
                self._set_failure("child_event_line_too_long")
                return
            event = _safe_child_event(expected_aliases, raw[:-1])
            if event is None:
                continue
            try:
                source_group = expected_aliases if isinstance(expected_aliases, str) else tuple(expected_aliases)
                self.events.put_nowait((source_group, event))
            except queue.Full:
                self._set_failure("child_event_queue_overflow")
                return

    def _set_failure(self, code: str) -> None:
        with self._failure_lock:
            if self._failure_code is None:
                self._failure_code = code

    def failure_code(self) -> str | None:
        with self._failure_lock:
            return self._failure_code

    def _spawn(self, aliases_for_process: list[str], leader: bool) -> subprocess.Popen[bytes]:
        command = [self.binary]
        if len(aliases_for_process) > 1:
            command.append("--diagnostic-two-alias")
        for alias in aliases_for_process:
            command.extend(["--alias", alias])
        command.extend(["--signal", self.signal_url])
        if leader:
            def create_group() -> None:
                os.setpgid(0, 0)

            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                preexec_fn=create_group,
                close_fds=True,
            )
            self.group = process.pid
        else:
            assert self.group is not None

            def join_group(group: int = self.group) -> None:
                os.setpgid(0, group)

            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                preexec_fn=join_group,
                close_fds=True,
            )
        self.processes.append(process)
        assert process.stdin is not None
        return process

    def start(self, ice: bytes) -> None:
        if not os.access(self.binary, os.X_OK):
            raise RuntimeError("controlled peer binary is not executable")
        for index, alias_group in enumerate(self.alias_groups):
            if _STOP.is_set():
                return
            process = self._spawn(alias_group, leader=index == 0)
            process.stdin.write(ice + b"\n")
            process.stdin.flush()
            process.stdin.close()

        for alias_group, process in zip(self.alias_groups, self.processes):
            reader = threading.Thread(target=self._read_output, args=(alias_group, process), daemon=True)
            reader.start()
            self.readers.append(reader)
        self.evidence.emit(
            {
                "event": "started",
                "mode": self.mode,
                "target_count": len(self.aliases),
                "aliases": self.aliases,
                "process_group": self.group,
            }
        )

    def _drain(self) -> list[tuple[str | tuple[str, ...], dict[str, Any]]]:
        events: list[tuple[str | tuple[str, ...], dict[str, Any]]] = []
        while True:
            try:
                events.append(self.events.get_nowait())
            except queue.Empty:
                return events

    def _apply_events(self, *, raise_failure: bool = True) -> None:
        if raise_failure:
            failure = self.failure_code()
            if failure is not None:
                raise RuntimeError(failure)
        for source_group, event in self._drain():
            kind = event["event"]
            if kind == "ready":
                event_alias = event.get("alias")
                if not isinstance(event_alias, str) or event_alias not in self.alias_group:
                    continue
                peer = event["peer"]
                if event_alias in self.ready and self.ready[event_alias] != peer:
                    raise RuntimeError(f"peer {event_alias} changed identity")
                existing_alias = next(
                    (ready_alias for ready_alias, ready_peer in self.ready.items() if ready_peer == peer),
                    None,
                )
                group_peer = next(
                    (
                        ready_peer
                        for ready_alias, ready_peer in self.ready.items()
                        if self.alias_group[ready_alias] == self.alias_group[event_alias]
                    ),
                    None,
                )
                if group_peer is not None and group_peer != peer:
                    raise RuntimeError("controlled peer aliases changed identity")
                if existing_alias is not None and existing_alias != event_alias:
                    if self.alias_group[existing_alias] != self.alias_group[event_alias]:
                        raise RuntimeError("duplicate controlled peer id")
                self.ready[event_alias] = peer
                self.evidence.emit({"event": "peer_ready", "alias": event_alias, "peer": peer})
            elif kind == "request":
                stats = self.stats[source_group] if isinstance(source_group, str) else self.group_stats[source_group]
                stats[event["kind"]] += 1
                stats["requests"] += 1
                stats["bytes"] += event["bytes"]
            elif kind == "fatal":
                if raise_failure:
                    raise RuntimeError(f"controlled peer {source_group} failed")
        if raise_failure:
            failure = self.failure_code()
            if failure is not None:
                raise RuntimeError(failure)

    def wait_ready(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while len(self.ready) < len(self.aliases):
            if _STOP.is_set():
                return False
            self._apply_events()
            if _STOP.is_set():
                return False
            for process in self.processes:
                if process.poll() is not None:
                    raise RuntimeError("controlled peer exited before readiness")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("controlled peers did not become ready")
            time.sleep(min(0.05, remaining))
        if _STOP.is_set():
            return False
        self.evidence.emit(
            {
                "event": "ready",
                "mode": self.mode,
                "target_count": len(self.aliases),
                "peer_count": len(set(self.ready.values())),
                "aliases": self.aliases,
            }
        )
        return True

    def run(self, duration: float | None) -> None:
        deadline = None if duration is None else time.monotonic() + duration
        while not self._stop_requested():
            self._apply_events()
            for process in self.processes:
                if process.poll() is not None:
                    raise RuntimeError("controlled peer exited unexpectedly")
            if deadline is not None and time.monotonic() >= deadline:
                return
            time.sleep(0.1)

    @staticmethod
    def _stop_requested() -> bool:
        return _STOP.is_set()

    def close(self) -> None:
        if self.group is not None:
            try:
                os.killpg(self.group, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline and any(p.poll() is None for p in self.processes):
                time.sleep(0.05)
            if any(p.poll() is None for p in self.processes):
                try:
                    os.killpg(self.group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for process in self.processes:
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
        for reader in self.readers:
            reader.join(timeout=1.0)
        self._apply_events(raise_failure=False)
        stats = {"event": "peer_stats", "mode": self.mode}
        if self.mode == "webrtc":
            stats["targets"] = self.stats
        else:
            stats["groups"] = [
                {"aliases": list(group), **self.group_stats[tuple(group)]}
                for group in self.alias_groups
            ]
        self.evidence.emit(stats)
        self.evidence.emit(
            {
                "event": "stopped",
                "mode": self.mode,
                "cleanup": "complete",
                "children_alive": sum(process.poll() is None for process in self.processes),
            }
        )


_STOP = threading.Event()


def _signal_handler(_signum: int, _frame: Any) -> None:
    _STOP.set()


def source_main(args: argparse.Namespace, evidence: Evidence) -> int:
    harness = SourceHarness(args.count, evidence)
    harness.start()
    try:
        deadline = None if args.duration is None else time.monotonic() + args.duration
        while not _STOP.is_set() and (deadline is None or time.monotonic() < deadline):
            time.sleep(0.1)
        return 0
    finally:
        harness.close()


def webrtc_main(
    args: argparse.Namespace,
    evidence: Evidence,
    alias_groups: list[list[str]] | None = None,
) -> int:
    mode = "webrtc-two-alias" if alias_groups is not None else "webrtc"
    try:
        ice = _read_ice_stdin()
    except ValueError as error:
        evidence.emit({"event": "error", "mode": mode, "code": "invalid_ice_input"})
        print(str(error), file=sys.stderr)
        return 2
    count = len(alias_groups) if alias_groups is not None else args.count
    if alias_groups is None:
        harness = PeerHarness(
            count,
            args.peer_binary,
            args.signal,
            evidence,
            getattr(args, "alias_prefix", "room.q2test.server"),
        )
    else:
        harness = PeerHarness(count, args.peer_binary, args.signal, evidence, alias_groups=alias_groups)
    try:
        harness.start(ice)
        if not harness.wait_ready(args.ready_timeout):
            return 0
        harness.run(args.duration)
        return 0
    except TimeoutError as error:
        if _STOP.is_set():
            return 0
        evidence.emit(
            {
                "event": "error",
                "mode": harness.mode,
                "code": "peer_readiness_timeout",
                "ready_count": len(harness.ready),
                "target_count": len(harness.aliases),
            }
        )
        print(str(error), file=sys.stderr)
        return 1
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        if _STOP.is_set():
            return 0
        evidence.emit(
            {
                "event": "error",
                "mode": harness.mode,
                "code": harness.failure_code() or "peer_harness_failed",
            }
        )
        print(str(error), file=sys.stderr)
        return 1
    finally:
        harness.close()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    source = subparsers.add_parser("source", help="serve deterministic HTTP/UDP targets")
    source.add_argument("--count", type=_safe_count, default=MAX_TARGETS)
    source.add_argument("--duration", type=_safe_duration)
    source.add_argument("--evidence", help="mirror JSONL evidence to this path")

    webrtc = subparsers.add_parser("webrtc", help="run controlled WebRTC-NET peers")
    webrtc.add_argument("--count", type=_safe_count, default=MAX_TARGETS)
    webrtc.add_argument("--alias-prefix", type=_safe_alias_prefix, default="room.q2test.server")
    webrtc.add_argument("--peer-binary", required=True)
    webrtc.add_argument("--signal", default=DEFAULT_SIGNAL)
    webrtc.add_argument("--ready-timeout", type=float, default=30.0)
    webrtc.add_argument("--duration", type=_safe_duration)
    webrtc.add_argument("--evidence", help="mirror JSONL evidence to this path")

    diagnostic = subparsers.add_parser(
        "webrtc-two-alias",
        help="run one diagnostic WebRTC-NET peer for two aliases",
    )
    diagnostic.add_argument("--peer-binary", required=True)
    diagnostic.add_argument("--alias-prefix", type=_safe_alias_prefix, default="room.q2test.server")
    diagnostic.add_argument("--signal", default=DEFAULT_SIGNAL)
    diagnostic.add_argument("--ready-timeout", type=float, default=30.0)
    diagnostic.add_argument("--duration", type=_safe_duration)
    diagnostic.add_argument("--evidence", help="mirror JSONL evidence to this path")
    return parser


def main() -> int:
    args = _parser().parse_args()
    if args.mode in {"webrtc", "webrtc-two-alias"} and args.ready_timeout <= 0:
        print("ready-timeout must be positive", file=sys.stderr)
        return 2
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)
    evidence = Evidence(args.evidence)
    try:
        if args.mode == "source":
            return source_main(args, evidence)
        if args.mode == "webrtc-two-alias":
            return webrtc_main(args, evidence, alias_groups=[aliases(2, args.alias_prefix)])
        return webrtc_main(args, evidence)
    finally:
        evidence.close()


if __name__ == "__main__":
    raise SystemExit(main())
