#!/usr/bin/env python3
import gzip
import importlib.util
import json
import os
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
LOADER = importlib.machinery.SourceFileLoader("q2dz_launch", str(ROOT / "tools" / "q2dz-launch"))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
assert SPEC
launch = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(launch)
CONTRACT_CASES = json.loads(
    (ROOT.parent / "tests" / "contracts" / "doszone-api-cases.json").read_text(encoding="utf-8")
)


class Response:
    def __init__(self, body, encoding=None, status=None):
        self._body = body
        self._offset = 0
        self.headers = {"Content-Encoding": encoding} if encoding else {}
        self.status = status

    def read(self, _size=-1):
        if _size is None or _size < 0:
            _size = len(self._body) - self._offset
        chunk = self._body[self._offset:self._offset + _size]
        self._offset += len(chunk)
        return chunk

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def getcode(self):
        return self.status


class _DeadlineSocket:
    def __init__(self):
        self.timeout = None

    def settimeout(self, timeout):
        self.timeout = timeout


class SlowResponse(Response):
    def __init__(self):
        super().__init__(b"")
        self.fp = type("FP", (), {"raw": type("Raw", (), {"_sock": _DeadlineSocket()})()})()

    def read(self, _size=-1):
        if self.fp.raw._sock.timeout is not None and self.fp.raw._sock.timeout < 0.1:
            raise socket.timeout("slow fake body")
        return b""


def room(room_id="room.q2.abc.123", owner="123", map_name="q2dm1"):
    return {
        "room": room_id,
        "owner": owner,
        "roomReady": True,
        "players": 1,
        "rtcServer": "lvc.dos.zone",
        "createSettings": {
            "prefix": "q2",
            "name": "The Edge",
            "map": map_name,
            "maxPlayers": 16,
        },
    }


class LauncherTests(unittest.TestCase):
    def test_shared_catalog_contract_cases(self):
        validators = {
            "rooms": launch.validate_room_payload,
            "aliases": launch.validate_alias_payload,
            "rtc": launch.validate_rtc_payload,
        }
        for case in CONTRACT_CASES:
            with self.subTest(case=case["name"]):
                self.assertEqual(
                    validators[case["kind"]](case["payload"]),
                    case["valid"],
                )
                if case["kind"] == "rooms":
                    try:
                        selected = launch.joinable_rooms(case["payload"])
                    except launch.LaunchError:
                        selected = []
                    if case["valid"]:
                        expected_room = case["payload"]["rooms"][0]["room"]
                        self.assertEqual([row["room"] for row in selected], [expected_room])
                    else:
                        self.assertEqual(selected, [])

    def test_mixed_catalog_selects_only_valid_q2_room(self):
        good = room("room.q2.good.1", "1")
        bad = {**room("room.q2.bad.1", "2"), "players": True}
        q3 = {**room("room.q3.ignore.1", "3"), "createSettings": {"prefix": "q3", "name": "Q3", "map": "q3dm1", "maxPlayers": 16}}
        selected = launch.joinable_rooms({"rooms": [good, bad, q3]})
        self.assertEqual([row["room"] for row in selected], ["room.q2.good.1"])

    def test_duplicate_room_ids_are_rejected_on_initial_and_fresh_selection(self):
        first = room("room.q2.duplicate.1", "1")
        second = room("room.q2.duplicate.1", "2")
        with self.assertRaises(launch.LaunchError):
            launch.joinable_rooms({"rooms": [first, second]})

    def test_help_never_starts_discovery_or_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [os.sys.executable, str(ROOT / "tools" / "q2dz-launch"), "--help"],
                input="",
                capture_output=True,
                text=True,
                check=False,
                env={"HOME": tmp, "XDG_CONFIG_HOME": tmp, "PATH": "/nonexistent"},
            )
        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage:", result.stdout)

    def test_select_q2_changes_path_without_fetching_rooms(self):
        with mock.patch.object(launch, "select_q2_path", return_value=Path("/games/Q2PRO-X.exe")) as selected, \
             mock.patch.object(launch, "fetch_json") as fetched:
            self.assertEqual(launch.main(["--select-q2"]), 0)
        selected.assert_called_once_with()
        fetched.assert_not_called()

    def test_fetch_json_accepts_bounded_gzip(self):
        payload = {"rooms": [room()]}
        body = gzip.compress(json.dumps(payload).encode())
        result = launch.fetch_json(
            launch.ROOM_ENDPOINT,
            opener=lambda _request, timeout: Response(body, "gzip"),
        )
        self.assertEqual(result["rooms"][0]["room"], "room.q2.abc.123")

    def test_fetch_json_rejects_malformed_json_and_oversized_decompressed_body(self):
        with self.assertRaises(launch.LaunchError):
            launch.fetch_json(
                launch.ROOM_ENDPOINT,
                opener=lambda _request, timeout: Response(b"{not-json"),
            )
        oversized = gzip.compress(b"x" * (launch.MAX_BODY + 1))
        with self.assertRaises(launch.LaunchError):
            launch.fetch_json(
                launch.ROOM_ENDPOINT,
                opener=lambda _request, timeout: Response(oversized, "gzip"),
            )

    def test_fetch_json_rejects_oversized_compressed_body(self):
        raw = os.urandom(launch.MAX_BODY)
        compressed = gzip.compress(raw)
        self.assertGreater(len(compressed), launch.MAX_BODY)
        with self.assertRaises(launch.LaunchError):
            launch.fetch_json(
                launch.ROOM_ENDPOINT,
                opener=lambda _request, timeout: Response(compressed, "gzip"),
            )

    def test_fetch_json_enforces_deadline_and_success_status(self):
        with self.assertRaises(launch.LaunchError):
            launch.fetch_json(
                launch.ROOM_ENDPOINT,
                opener=lambda _request, timeout: SlowResponse(),
                timeout=0.01,
            )
        for status in (302, 500):
            with self.subTest(status=status), self.assertRaises(launch.LaunchError):
                launch.fetch_json(
                    launch.ROOM_ENDPOINT,
                    opener=lambda _request, timeout, status=status: Response(b"{}", status=status),
                )

    def test_fetch_json_rejects_unsupported_content_encoding(self):
        for encoding in ("br", "gzip, gzip"):
            with self.subTest(encoding=encoding), self.assertRaises(launch.LaunchError):
                launch.fetch_json(
                    launch.ROOM_ENDPOINT,
                    opener=lambda _request, timeout, encoding=encoding: Response(b"{}", encoding),
                )

    def test_room_filter_is_q2_ready_and_not_full(self):
        rows = [
            room(),
            {**room("room.q3.nope.1"), "createSettings": {"prefix": "q3", "name": "q3", "map": "q3dm1", "maxPlayers": 16}},
            {**room("room.q2.full.2"), "players": 16},
            {**room("room.q2.not-ready.3"), "roomReady": False},
        ]
        self.assertEqual([r["room"] for r in launch.joinable_rooms({"rooms": rows})], ["room.q2.abc.123"])

    def test_unsupported_catalog_contract_stops_before_rtc_fetch_or_session(self):
        conflicts = (("prefix", "q3"), ("game", "baseq2"), ("profile", "baseq2"), ("ordered", True))
        for field, value in conflicts:
            with self.subTest(field=field):
                selected = room()
                selected["createSettings"] = {**selected["createSettings"], field: value}
                endpoints = []

                def fake_fetch(endpoint):
                    endpoints.append(endpoint)
                    if endpoint == launch.ROOM_ENDPOINT:
                        return {"rooms": [selected]}
                    if endpoint == launch.LOOKUP_ENDPOINT:
                        return {"aliases": ["q2.123"]}
                    return [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]

                with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
                     mock.patch.object(launch, "fetch_json", side_effect=fake_fetch), \
                     mock.patch.object(launch, "notify"):
                    self.assertEqual(launch.main([]), 0)
                self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT])

    def test_invalid_snapshot_stops_before_rtc_fetch_or_session(self):
        selected = room()
        conflicting = {**selected, "createSettings": {**selected["createSettings"], "profile": "baseq2"}}
        endpoints = []

        def fake_fetch(endpoint):
            endpoints.append(endpoint)
            if endpoint == launch.ROOM_ENDPOINT:
                return {"rooms": [conflicting]}
            if endpoint == launch.LOOKUP_ENDPOINT:
                return {"aliases": ["q2.123"]}
            return [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]

        with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
             mock.patch.object(launch, "fetch_json", side_effect=fake_fetch), \
             mock.patch.object(launch, "notify"):
            self.assertEqual(launch.main([]), 0)
        self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT])

    def test_room_list_hides_stale_unregistered_aliases(self):
        rows = [room("room.q2.live.1", "123"), room("room.q2.stale.2", "456")]
        visible = launch.registered_rooms(rows, {"aliases": ["q2.123"]})
        self.assertEqual([row["room"] for row in visible], ["room.q2.live.1"])

    def test_alias_matching_is_case_sensitive(self):
        rows = [room("room.q2.live.1", "123")]
        self.assertEqual(launch.registered_rooms(rows, {"aliases": ["Q2.123"]}), [])
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(
                rows[0],
                {"aliases": ["Q2.123"]},
                [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
            )

    def test_room_ids_differing_only_by_case_are_distinct(self):
        upper = room("ROOM.Q2.CASE.1", "123")
        lower = room("room.q2.case.1", "456")
        self.assertEqual(
            len(launch.joinable_rooms({"rooms": [upper, lower]})),
            2,
        )

    def test_desktop_error_uses_zenity(self):
        calls = []

        def fake_run(args, **_kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0)

        with mock.patch.dict(os.environ, {"DISPLAY": ":1"}, clear=False):
            launch.notify("room unavailable", zenity="/usr/bin/zenity", runner=fake_run)
        self.assertEqual(calls[0][:2], ["/usr/bin/zenity", "--error"])
        self.assertIn("--text=room unavailable", calls[0])

    def test_room_config_requires_alias_and_fresh_rtc(self):
        selected = room()
        lookup = {"aliases": ["q2.123"]}
        rtc = [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]
        config = launch.make_room_config(selected, lookup, rtc)
        self.assertEqual(config["alias"], "q2.123")
        self.assertFalse(config["ordered"])
        self.assertEqual(config["map"], "q2dm1")
        self.assertEqual(config["iceServers"], rtc[0]["iceservers"])
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(selected, {"aliases": []}, rtc)

    def test_room_config_rejects_alias_mismatch_hostname_and_ordered_rtc(self):
        selected = room()
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(
                selected,
                {"aliases": ["q2.other"]},
                [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
            )
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(
                selected,
                {"aliases": ["q2.123"]},
                [{"domain": "https://lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
            )
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(
                selected,
                {"aliases": ["q2.123"]},
                [{"domain": "lvc.dos.zone", "ordered": True,
                  "iceservers": [{"urls": ["stun:example.invalid"]}]}],
            )

    def test_hub_command_uses_composite_source_and_no_connect(self):
        configs = [
            {"version": 1, "roomId": "room.q2.a.1", "alias": "q2.1",
             "ordered": False, "iceServers": [], "map": "q2dm1", "game": "3zb2"},
            {"version": 1, "roomId": "room.q2.b.2", "alias": "q2.2",
             "ordered": False, "iceServers": [], "map": "ztn2dm3", "game": "3zb2"},
        ]
        calls = []

        def fake_run(args, **kwargs):
            calls.append((args, kwargs))
            return subprocess.CompletedProcess(args, 0)

        rc = launch.run_hub(
            configs,
            "/games/Q2PRO-X.exe",
            session="/opt/q2dz/bin/q2dz-session",
            managed="/opt/q2dz/bin/q2dz-managed-gateway",
            legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
            adapter="/opt/q2dz/config/legacy-gateway.json",
            runner=fake_run,
        )
        self.assertEqual(rc, 0)
        args, kwargs = calls[0]
        self.assertNotIn("+connect", args)
        self.assertNotIn("+connect", kwargs["input"])
        self.assertEqual(kwargs["input"].count("\n"), 2)

    def test_hub_command_keeps_room_data_out_of_argv(self):
        config = {
            "version": 1,
            "roomId": "room.q2.abc.123",
            "alias": "q2.123",
            "ordered": False,
            "iceServers": [{"urls": ["turn:example.invalid"], "username": "u", "credential": "secret"}],
        }
        q2 = "/games/Q2PRO-X.exe"
        calls = []

        def fake_run(args, **kwargs):
            calls.append((args, kwargs))
            return subprocess.CompletedProcess(args, 0)

        rc = launch.run_hub(
            [config],
            q2,
            session="/opt/q2dz/bin/q2dz-session",
            managed="/opt/q2dz/bin/q2dz-managed-gateway",
            legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
            adapter="/opt/q2dz/config/legacy-gateway.json",
            runner=fake_run,
        )
        self.assertEqual(rc, 0)
        args, kwargs = calls[0]
        self.assertEqual(args[0], "/opt/q2dz/bin/q2dz-session")
        self.assertIn("--legacy-gateway", args)
        self.assertEqual(args[args.index("--game"):args.index("--game") + 2], ["--game", "3zb2"])
        self.assertNotIn("secret", args)
        self.assertEqual(json.loads(kwargs["input"])["iceServers"][0]["credential"], "secret")
        self.assertNotIn("shell", kwargs)

    def test_hub_snapshot_takes_first_two_compatible_rooms_without_selection(self):
        rows = [room("room.q2.one.1", "1", "q2dm1"),
                room("room.q2.two.2", "2", "missing"),
                room("room.q2.three.3", "3", "ztn2dm3")]
        lookup = {"aliases": ["q2.1", "q2.2", "q2.3"]}
        rtc = [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]
        with mock.patch.object(launch, "content_ready", side_effect=[True, True, False]):
            configs = launch.make_hub_configs({"rooms": rows}, lookup, rtc, "/games/Q2PRO-X.exe")
        self.assertEqual([config["roomId"] for config in configs], ["room.q2.one.1", "room.q2.three.3"])

    def test_hub_content_checks_are_bounded_before_subprocess_fanout(self):
        rows = [room(f"room.q2.{index}.1", str(index), "missing") for index in range(12)]
        lookup = {"aliases": [f"q2.{index}" for index in range(12)]}
        rtc = [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]
        with mock.patch.object(launch, "content_ready", return_value=False) as checked:
            with self.assertRaises(launch.LaunchError):
                launch.make_hub_configs({"rooms": rows}, lookup, rtc, "/games/Q2PRO-X.exe")
        self.assertEqual(checked.call_count, launch.HUB_MAX_CONTENT_CHECKS)

    def test_path_store_persists_only_q2_path(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as game_tmp:
            path = Path(game_tmp) / "Q2PRO-X.exe"
            path.write_bytes(b"client")
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": tmp}, clear=False):
                launch.save_q2_path(path)
                stored = launch.load_q2_path()
            self.assertEqual(stored, path)
            files = [p.relative_to(tmp).as_posix() for p in Path(tmp).rglob("*") if p.is_file()]
            self.assertEqual(files, ["q2pro-x-doszone/q2-path"])

    def test_missing_client_offers_download_without_starting_file_picker(self):
        calls = []

        def fake_run(args, **_kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0, stdout="Open download page\n")

        with mock.patch.object(launch.webbrowser, "open") as opened:
            self.assertIsNone(launch.select_q2_path(zenity="/usr/bin/zenity", runner=fake_run))
        self.assertIn("--question", calls[0])
        self.assertEqual(len(calls), 1)
        opened.assert_called_once_with(launch.OFFICIAL_URL)

    def test_session_failure_is_visible_from_desktop_launcher(self):
        selected = room()
        replies = [
            {"rooms": [selected]},
            {"aliases": ["q2.123"]},
            [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
        ]
        with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
             mock.patch.object(launch, "fetch_json", side_effect=replies), \
             mock.patch.object(launch, "content_ready", return_value=True), \
             mock.patch.object(launch, "run_hub", return_value=71), \
             mock.patch.object(launch, "notify") as notified:
            self.assertEqual(launch.main([]), 71)
        notified.assert_called_once_with("Dos.Zone session ended unexpectedly (code 71).")

    def test_room_close_is_reported_as_normal(self):
        selected = room()
        replies = [
            {"rooms": [selected]},
            {"aliases": ["q2.123"]},
            [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
        ]
        with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
             mock.patch.object(launch, "fetch_json", side_effect=replies), \
             mock.patch.object(launch, "content_ready", return_value=True), \
             mock.patch.object(launch, "run_hub", return_value=73), \
             mock.patch.object(launch, "notify") as notified:
            self.assertEqual(launch.main([]), 73)
        notified.assert_called_once_with("The Dos.Zone room was closed.", "info")


if __name__ == "__main__":
    unittest.main()
