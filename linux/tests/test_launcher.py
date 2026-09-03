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


def dedicated_room(room_id="room.q2ded.server.1", map_name="q2dm1"):
    return {
        "room": room_id,
        "owner": 0,
        "players": 1,
        "createSettings": {
            "prefix": "q2ded",
            "name": "Server",
            "map": map_name,
            "maxPlayers": 16,
        },
    }


def slots5(q2a=None, q2b=None, dedicated=None):
    slots = [launch.HubSlot() for _ in range(launch.HUB_MAX_ROOMS)]
    if dedicated:
        slots[0] = dedicated
    if q2a:
        slots[3] = q2a
    if q2b:
        slots[4] = q2b
    return slots


class LauncherTests(unittest.TestCase):
    def test_live_slots_keep_existing_room_and_fill_free_slot(self):
        slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        rows = [room("room.a", "1"), room("room.b", "2")]

        def make_config(row):
            return row["room"].split(".")[-1].upper()

        updated = launch.reconcile_slots(slots, rows, make_config)
        self.assertEqual(
            [(slot.room_id, slot.config) for slot in updated[3:]],
            [("room.a", "A"), ("room.b", "B")],
        )

    def test_one_missing_poll_does_not_remove_room(self):
        slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        updated = launch.reconcile_slots(slots, [], lambda _row: "unused")
        self.assertEqual(updated[3], launch.HubSlot("room.a", "A", 1))

    def test_ready_rooms_excludes_not_ready_rooms(self):
        not_ready = room("room.q2.not-ready.1", "1")
        not_ready["roomReady"] = False
        self.assertEqual(launch.ready_rooms({"rooms": [not_ready]}), [])

    def test_retained_room_false_readiness_clears_after_two_successful_polls(self):
        not_ready = room("room.q2.not-ready.1", "1")
        not_ready["roomReady"] = False
        rows = launch.ready_rooms({"rooms": [not_ready]})
        slots = slots5(q2a=launch.HubSlot("room.q2.not-ready.1", "A", 0))
        first = launch.reconcile_slots(slots, rows, lambda _row: "unused")
        second = launch.reconcile_slots(first, rows, lambda _row: "unused")
        self.assertEqual(first[3], launch.HubSlot("room.q2.not-ready.1", "A", 1))
        self.assertEqual(second[3], launch.HubSlot())

    def test_catalog_validation_checks_not_ready_q2_rows_and_duplicates(self):
        not_ready = room("room.q2.not-ready.1", "1")
        not_ready["roomReady"] = False
        self.assertTrue(launch.validate_room_payload({"rooms": [not_ready]}))
        invalid = room("room.q2.invalid.2", "2")
        invalid["createSettings"]["map"] = "bad map"
        self.assertFalse(launch.validate_room_payload({"rooms": [not_ready, invalid]}))
        duplicate = room("room.q2.not-ready.1", "2")
        duplicate["roomReady"] = False
        self.assertFalse(launch.validate_room_payload({"rooms": [not_ready, duplicate]}))

    def test_two_missing_polls_free_slot(self):
        slots = slots5(q2a=launch.HubSlot("room.a", "A", 1))
        updated = launch.reconcile_slots(slots, [], lambda _row: "unused")
        self.assertEqual(updated[3], launch.HubSlot())

    def test_catalog_failure_sends_no_commands_and_preserves_slots(self):
        existing_slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        writer = mock.Mock()

        def fetch_raises():
            raise launch.LaunchError("catalog unavailable")

        self.assertEqual(
            launch.poll_once(existing_slots, fetch_raises, writer),
            existing_slots,
        )
        writer.assert_not_called()

    def test_successful_poll_writes_heartbeat_even_when_slots_do_not_change(self):
        existing_slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        writer = mock.Mock()
        self.assertEqual(
            launch.poll_once(existing_slots, lambda: existing_slots, writer),
            existing_slots,
        )
        writer.assert_called_once_with(existing_slots)

    def test_retained_room_is_not_removed_when_it_becomes_full(self):
        slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        full = room("room.a", "1")
        full["players"] = full["createSettings"]["maxPlayers"]
        updated = launch.reconcile_slots(slots, [full], lambda _row: "unused")
        self.assertEqual(updated[3], launch.HubSlot("room.a", "A", 0, "q2dm1"))

    def test_new_full_room_does_not_fill_free_slot(self):
        full = room("room.b", "2")
        full["players"] = full["createSettings"]["maxPlayers"]
        slots = slots5()
        self.assertEqual(
            launch.reconcile_slots(slots, [full], lambda _row: "unused"),
            slots5(),
        )

    def test_three_rooms_fill_only_two_slots_without_evicting_retained_room(self):
        slots = slots5(q2a=launch.HubSlot("room.b", "B", 0))
        rows = [room("room.a", "1"), room("room.b", "2"), room("room.c", "3")]
        updated = launch.reconcile_slots(
            slots,
            rows,
            lambda row: row["room"].split(".")[-1].upper(),
        )
        self.assertEqual(
            [(slot.room_id, slot.config) for slot in updated[3:]],
            [("room.b", "B"), ("room.a", "A")],
        )

    def test_slot_commands_are_independent_and_bounded(self):
        self.assertEqual(launch.encode_slot(0, launch.HubSlot()), "0 -\n")
        self.assertEqual(launch.encode_slot(4, launch.HubSlot()), "4 -\n")
        with self.assertRaises(launch.LaunchError):
            launch.encode_slot(5, launch.HubSlot())
        self.assertEqual(
            launch.encode_slot(0, launch.HubSlot("room.a", "{\"map\":\"q2dm1\"}")),
            '0 room.a {"map":"q2dm1"}\n',
        )
        with self.assertRaises(launch.LaunchError):
            launch.encode_slot(
                0,
                launch.HubSlot("room.a", "x" * launch.MAX_HUB_COMMAND_BYTES),
            )

    def test_oversized_new_config_does_not_disturb_retained_room(self):
        slots = slots5(q2a=launch.HubSlot("room.a", "A", 0))
        rows = [room("room.a", "1"), room("room.b", "2")]

        def make_config(row):
            return "A" if row["room"] == "room.a" else "x" * launch.MAX_HUB_COMMAND_BYTES

        self.assertEqual(
            launch.reconcile_slots(slots, rows, make_config),
            slots5(q2a=launch.HubSlot("room.a", "A", 0, "q2dm1")),
        )

    def test_refresh_does_not_fetch_rtc_or_check_content_without_free_slot(self):
        rows = [room("room.q2.a.1", "1"), room("room.q2.b.2", "2")]
        replies = {
            launch.ROOM_ENDPOINT: {"rooms": rows},
            launch.LOOKUP_ENDPOINT: {"aliases": ["q2.1", "q2.2"]},
        }
        endpoints = []

        def fetch(endpoint):
            endpoints.append(endpoint)
            return replies[endpoint]

        slots = slots5(
            q2a=launch.HubSlot("room.q2.a.1", "A", 0, "q2dm1"),
            q2b=launch.HubSlot("room.q2.b.2", "B", 0, "q2dm1"),
        )
        with mock.patch.object(launch, "content_ready") as checked:
            updated = launch.refresh_slots(slots, "/games/Q2PRO-X.exe", fetch=fetch)
        self.assertEqual(updated, slots)
        self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT])
        checked.assert_not_called()

    def test_refresh_fetches_rtc_only_when_filling_a_free_slot(self):
        selected = room("room.q2.a.1", "1")
        replies = {
            launch.ROOM_ENDPOINT: {"rooms": [selected]},
            launch.LOOKUP_ENDPOINT: {"aliases": ["q2.1"]},
            launch.RTC_ENDPOINT: [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
        }
        endpoints = []

        def fetch(endpoint):
            endpoints.append(endpoint)
            return replies[endpoint]

        slots = slots5()
        with mock.patch.object(launch, "content_ready", return_value=True) as checked:
            updated = launch.refresh_slots(slots, "/games/Q2PRO-X.exe", fetch=fetch)
        self.assertEqual([slot.room_id for slot in updated[3:]], [selected["room"], ""])
        self.assertEqual(
            endpoints,
            [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT, launch.RTC_ENDPOINT],
        )
        checked.assert_called_once_with(
            "/games/Q2PRO-X.exe", "q2dm1", None, runner=subprocess.run
        )

    def test_shared_catalog_contract_cases(self):
        validators = {
            "rooms": launch.validate_room_payload,
            "aliases": launch.validate_alias_payload,
            "rtc": launch.validate_rtc_payload,
        }
        for case in CONTRACT_CASES:
            with self.subTest(case=case["name"]):
                if case["kind"] == "dedicated-ice":
                    try:
                        ice_servers = launch._dedicated_ice_servers(case["payload"])
                    except launch.LaunchError:
                        self.assertFalse(case["valid"])
                    else:
                        self.assertTrue(case["valid"])
                        self.assertEqual(ice_servers, case["expected"]["iceServers"])
                    continue
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
                        if "expected" in case:
                            row = case["payload"]["rooms"][0]
                            self.assertEqual(launch._room_alias(row), case["expected"]["alias"])
                            self.assertEqual(
                                [i for i in range(launch.HUB_MAX_ROOMS) if launch._slot_kind(i) == launch._room_kind(row)],
                                list(range(case["expected"]["slotRange"][0], case["expected"]["slotRange"][1] + 1)),
                            )
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

    def test_duplicate_room_ids_are_rejected_before_readiness_filter(self):
        ready = room("room.q2.duplicate.1", "1")
        not_ready = room("room.q2.duplicate.1", "2")
        not_ready["roomReady"] = False
        with self.assertRaises(launch.LaunchError):
            launch.joinable_rooms({"rooms": [ready, not_ready]})

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
                     mock.patch.object(launch, "run_hub", return_value=0) as run_hub, \
                     mock.patch.object(launch, "notify"):
                    self.assertEqual(launch.main([]), 0)
                self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT])
                run_hub.assert_called_once()

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
             mock.patch.object(launch, "run_hub", return_value=0) as run_hub, \
             mock.patch.object(launch, "notify"):
            self.assertEqual(launch.main([]), 0)
        self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT])
        run_hub.assert_called_once()

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

    def test_dedicated_room_uses_exact_alias_and_all_deduplicated_ice(self):
        selected = dedicated_room()
        duplicate = {"urls": ["stun:shared.example.invalid"]}
        rtc = [
            {"domain": "a.example.invalid", "iceservers": [duplicate, {"urls": ["turn:a.example.invalid"], "username": "u", "credential": "c"}]},
            {"domain": "b.example.invalid", "iceservers": [dict(duplicate), {"urls": ["stun:b.example.invalid"]}]},
        ]
        config = launch.make_room_config(selected, {"aliases": [selected["room"]]}, rtc)
        self.assertEqual(config["alias"], selected["room"])
        self.assertEqual(config["iceServers"], [rtc[0]["iceservers"][0], rtc[0]["iceservers"][1], rtc[1]["iceservers"][1]])
        self.assertFalse(config["ordered"])
        self.assertEqual(config["game"], "3zb2")

    def test_excess_dedicated_ice_does_not_block_q2_reconcile(self):
        legacy = room()
        dedicated = dedicated_room()
        rtc = [
            {"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:legacy.example.invalid"]}]},
            {"domain": "bulk.example.invalid", "iceservers": [
                {"urls": [f"stun:s{i}.example.invalid"]} for i in range(16)
            ]},
        ]
        replies = {
            launch.ROOM_ENDPOINT: {"rooms": [dedicated, legacy]},
            launch.LOOKUP_ENDPOINT: {"aliases": [dedicated["room"], "q2.123"]},
            launch.RTC_ENDPOINT: rtc,
        }
        updated = launch.refresh_slots(
            slots5(), "/games/Q2PRO-X.exe",
            fetch=lambda endpoint: replies[endpoint],
            content_checker="/fake/content-check",
            runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        self.assertEqual([slot.room_id for slot in updated[:3]], ["", "", ""])
        self.assertEqual(updated[3].room_id, legacy["room"])

    def test_dedicated_config_rejects_oversized_json(self):
        selected = dedicated_room()
        rtc = [{"domain": "a.example.invalid", "iceservers": [
            {"urls": [f"turn:s{i}.example.invalid?x={'x' * 1600}{j}" for j in range(16)]}
            for i in range(3)
        ]}]
        self.assertEqual(len(launch._dedicated_ice_servers(rtc)), 3)
        with self.assertRaises(launch.LaunchError):
            launch.make_room_config(selected, {"aliases": [selected["room"]]}, rtc)

    def test_dedicated_slots_are_isolated_from_q2_slots(self):
        rows = [dedicated_room(f"room.q2ded.server.{i}") for i in range(4)] + [
            room(f"room.q2.client.{i}", str(i)) for i in range(3)
        ]
        updated = launch.reconcile_slots(
            slots5(), rows, lambda row: {"roomId": row["room"], "map": row["createSettings"]["map"]}
        )
        self.assertEqual([slot.room_id for slot in updated[:3]], [row["room"] for row in rows[:3]])
        self.assertEqual([slot.room_id for slot in updated[3:]], [row["room"] for row in rows[4:6]])

    def test_retained_dedicated_refreshes_config_and_rechecks_changed_map(self):
        selected = dedicated_room(map_name="q2dm2")
        old = launch.HubSlot(selected["room"], '{"map":"q2dm1"}', 0, "q2dm1")
        checked = mock.Mock(return_value=True)
        updated = launch.reconcile_slots(
            slots5(dedicated=old),
            [selected],
            lambda row: {"roomId": row["room"], "map": row["createSettings"]["map"]},
            checked,
        )
        checked.assert_called_once_with(selected)
        self.assertEqual(updated[0].map_name, "q2dm2")
        self.assertIn('"q2dm2"', updated[0].config)

        checked.return_value = False
        removed = launch.reconcile_slots(slots5(dedicated=old), [selected], lambda _row: "unused", checked)
        self.assertEqual(removed[0], launch.HubSlot())

    def test_failed_dedicated_rtc_refresh_preserves_live_slots_and_sends_nothing(self):
        selected = dedicated_room()
        current = slots5(dedicated=launch.HubSlot(selected["room"], "old", 0, "q2dm1"))
        replies = {
            launch.ROOM_ENDPOINT: {"rooms": [selected]},
            launch.LOOKUP_ENDPOINT: {"aliases": [selected["room"]]},
            launch.RTC_ENDPOINT: [],
        }
        writer = mock.Mock()
        result = launch.poll_once(
            current,
            lambda: launch.refresh_slots(current, "/games/Q2PRO-X.exe", fetch=lambda endpoint: replies[endpoint]),
            writer,
        )
        self.assertEqual(result, current)
        writer.assert_not_called()

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

        class FakeInput:
            def __init__(self):
                self.payload = ""

            def write(self, value):
                self.payload += value

            def flush(self):
                pass

            def close(self):
                pass

        class FakeProcess:
            def __init__(self):
                self.stdin = FakeInput()

            def wait(self, timeout=None):
                return 0

            def poll(self):
                return 0

        def fake_popen(args, **kwargs):
            calls.append((args, kwargs))
            process = FakeProcess()
            calls[-1] = (args, {**kwargs, "process": process})
            return process

        rc = launch.run_hub(
            configs,
            "/games/Q2PRO-X.exe",
            session="/opt/q2dz/bin/q2dz-session",
            managed="/opt/q2dz/bin/q2dz-managed-gateway",
            legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
            adapter="/opt/q2dz/config/legacy-gateway.json",
            popen=fake_popen,
        )
        self.assertEqual(rc, 0)
        args, kwargs = calls[0]
        self.assertNotIn("+connect", args)
        self.assertNotIn("+connect", kwargs["process"].stdin.payload)
        self.assertEqual(kwargs["process"].stdin.payload.count("\n"), 5)

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

        class FakeInput:
            def __init__(self):
                self.payload = ""

            def write(self, value):
                self.payload += value

            def flush(self):
                pass

            def close(self):
                pass

        class FakeProcess:
            def __init__(self):
                self.stdin = FakeInput()

            def wait(self, timeout=None):
                return 0

            def poll(self):
                return 0

        def fake_popen(args, **kwargs):
            calls.append((args, kwargs))
            process = FakeProcess()
            calls[-1] = (args, {**kwargs, "process": process})
            return process

        rc = launch.run_hub(
            [config],
            q2,
            session="/opt/q2dz/bin/q2dz-session",
            managed="/opt/q2dz/bin/q2dz-managed-gateway",
            legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
            adapter="/opt/q2dz/config/legacy-gateway.json",
            popen=fake_popen,
        )
        self.assertEqual(rc, 0)
        args, kwargs = calls[0]
        self.assertEqual(args[0], "/opt/q2dz/bin/q2dz-session")
        self.assertIn("--legacy-gateway", args)
        self.assertEqual(args[args.index("--game"):args.index("--game") + 2], ["--game", "3zb2"])
        self.assertNotIn("secret", args)
        line = kwargs["process"].stdin.payload.splitlines()[3]
        self.assertEqual(json.loads(line.split(" ", 2)[2])["iceServers"][0]["credential"], "secret")
        self.assertNotIn("shell", kwargs)

    def test_coordinator_exit_before_reading_is_returned_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            coordinator = Path(tmp) / "coordinator.py"
            coordinator.write_text("#!/usr/bin/env python3\nimport sys\nsys.exit(74)\n", encoding="utf-8")
            coordinator.chmod(0o755)
            rc = launch.run_hub(
                [],
                "/games/Q2PRO-X.exe",
                session=coordinator,
                managed="/opt/q2dz/bin/q2dz-managed-gateway",
                legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
                adapter="/opt/q2dz/config/legacy-gateway.json",
            )
        self.assertEqual(rc, 74)

    def test_coordinator_exit_after_first_command_is_returned_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            coordinator = Path(tmp) / "coordinator.py"
            coordinator.write_text(
                "#!/usr/bin/env python3\nimport sys\nsys.stdin.readline()\nsys.exit(74)\n",
                encoding="utf-8",
            )
            coordinator.chmod(0o755)
            rc = launch.run_hub(
                [],
                "/games/Q2PRO-X.exe",
                session=coordinator,
                managed="/opt/q2dz/bin/q2dz-managed-gateway",
                legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
                adapter="/opt/q2dz/config/legacy-gateway.json",
            )
        self.assertEqual(rc, 74)

    def test_hub_polls_after_timeout_and_sends_heartbeat(self):
        class FakeInput:
            def __init__(self):
                self.payload = ""

            def write(self, value):
                self.payload += value

            def flush(self):
                pass

            def close(self):
                pass

        class FakeProcess:
            def __init__(self):
                self.stdin = FakeInput()
                self.waits = 0

            def wait(self, timeout=None):
                self.waits += 1
                if self.waits == 1:
                    raise subprocess.TimeoutExpired("fake", timeout)
                return 74

            def poll(self):
                return 74

        processes = []

        def fake_popen(_args, **_kwargs):
            process = FakeProcess()
            processes.append(process)
            return process

        slots = slots5()
        rc = launch.run_hub(
            slots,
            "/games/Q2PRO-X.exe",
            session="/opt/q2dz/bin/q2dz-session",
            managed="/opt/q2dz/bin/q2dz-managed-gateway",
            legacy="/opt/q2dz/bin/q2dz-webrtc-gateway",
            adapter="/opt/q2dz/config/legacy-gateway.json",
            popen=fake_popen,
            refresh=lambda current, _q2: current,
            poll_interval=0,
        )
        self.assertEqual(rc, 74)
        self.assertEqual(
            processes[0].stdin.payload,
            "0 -\n1 -\n2 -\n3 -\n4 -\n0 -\n1 -\n2 -\n3 -\n4 -\n",
        )

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
        self.assertEqual(checked.call_count, launch.HUB_Q2_SLOTS * launch.HUB_CANDIDATES_PER_SLOT)

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
