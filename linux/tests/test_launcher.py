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

    def read(self, size=-1):
        if size is None or size < 0:
            size = len(self._body) - self._offset
        chunk = self._body[self._offset:self._offset + size]
        self._offset += len(chunk)
        return chunk

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def getcode(self):
        return self.status


class SlowResponse(Response):
    def __init__(self):
        super().__init__(b"")
        self.fp = type("FP", (), {"raw": type("Raw", (), {"_sock": self})()})()
        self.timeout = None

    def settimeout(self, timeout):
        self.timeout = timeout

    def read(self, _size=-1):
        if self.timeout is not None and self.timeout < 0.1:
            raise socket.timeout("slow fake body")
        return b""


def room(room_id="room.q2.abc.123", owner="123", map_name="q2dm1", name="The Edge"):
    return {
        "room": room_id,
        "owner": owner,
        "roomReady": True,
        "players": 1,
        "rtcServer": "lvc.dos.zone",
        "createSettings": {
            "prefix": "q2",
            "name": name,
            "map": map_name,
            "maxPlayers": 16,
        },
    }


def dedicated_room(room_id="room.q2ded.server.1", map_name="q2dm1", name="Server"):
    return {
        "room": room_id,
        "owner": 0,
        "players": 1,
        "createSettings": {
            "prefix": "q2ded",
            "name": name,
            "map": map_name,
            "maxPlayers": 16,
        },
    }


def rtc_rows():
    return [{"domain": "lvc.dos.zone", "iceservers": [{"urls": ["stun:example.invalid"]}]}]


def catalog(rows):
    return {"rooms": rows}


def lookup_for(rows):
    return {"aliases": [launch._room_alias(row) for row in rows]}


def scenario_rows(payload):
    rows = []
    for index in range(payload.get("q2", 0)):
        rows.append(room(f"room.q2.client.{index:03d}", str(index), name=f"Q2 {index:03d}"))
    for index in range(payload.get("q2ded", 0)):
        rows.append(dedicated_room(f"room.q2ded.server.{index:03d}", name=f"DED {index:03d}"))
    return rows


def generated_oversized_rtc(payload):
    servers = []
    for server_index in range(payload["serverCount"]):
        urls = []
        for url_index in range(payload["urlsPerServer"]):
            prefix = f"stun:example.invalid?server={server_index}&url={url_index}&"
            urls.append(prefix + "x" * (payload["urlLength"] - len(prefix)))
        servers.append({"urls": urls})
    return [{"domain": "lvc.dos.zone", "iceservers": servers}]


class FakeInput:
    def __init__(self):
        self.payload = bytearray()

    def write(self, value):
        self.payload.extend(value)

    def flush(self):
        pass

    def close(self):
        pass


class FakeProcess:
    def __init__(self, waits_to_exit=2, exit_code=0):
        self.stdin = FakeInput()
        self.waits_to_exit = waits_to_exit
        self.waits = 0
        self.exit_code = exit_code

    def wait(self, timeout=None):
        self.waits += 1
        if self.waits >= self.waits_to_exit:
            return self.exit_code
        raise subprocess.TimeoutExpired("fake", timeout)

    def poll(self):
        return self.exit_code if self.waits >= self.waits_to_exit else None


def decoded_lines(process):
    return process.stdin.payload.decode("utf-8").splitlines()


class LauncherTests(unittest.TestCase):
    def _execute_heartbeat_scenario(self, case):
        name = case["name"]
        payload = case.get("payload", {})
        if name in {"heartbeat-mixed-20-q2-20-q2ded", "heartbeat-retains-all-40"}:
            rows = scenario_rows(payload)
            plan = launch.refresh_targets(
                {}, "/games/Q2PRO-X.exe",
                fetch=lambda endpoint: {
                    launch.ROOM_ENDPOINT: catalog(rows),
                    launch.LOOKUP_ENDPOINT: lookup_for(rows),
                    launch.RTC_ENDPOINT: rtc_rows(),
                }[endpoint],
                content_checker="unused",
                runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
            )
            if name == "heartbeat-mixed-20-q2-20-q2ded":
                process = FakeProcess()
                launch.write_heartbeat(process, plan, generation=1)
                put_ids = [
                    line.split(" room=", 1)[1].split(" json=", 1)[0]
                    for line in decoded_lines(process)
                    if " PUT " in line
                ]
                return {
                    "count": len(plan.targets),
                    "q2": sum(row["createSettings"]["prefix"] == "q2" for row in rows),
                    "q2ded": sum(row["createSettings"]["prefix"] == "q2ded" for row in rows),
                    "order": "room-id" if put_ids == sorted(put_ids) else "unexpected",
                }
            first_miss = launch.reconcile_targets(plan.targets, [], lambda _row: "unused")
            second_miss = launch.reconcile_targets(first_miss.targets, [], lambda _row: "unused")
            return {
                "count": len(plan.targets),
                "missesBeforeDrop": 2 if not second_miss.targets and len(second_miss.drops) == len(plan.targets) else 0,
            }

        if name == "heartbeat-map-content-failure-preserves-target":
            old = room("room.q2.map-scenario.1", "1", map_name=payload["mapBefore"])
            changed = room(old["room"], "1", map_name=payload["mapAfter"])
            old_config = launch.make_room_config(old, lookup_for([old]), rtc_rows())
            current = {
                old["room"]: launch.HubTarget(
                    old["room"], launch._config_text(old_config), 0, payload["mapBefore"]
                )
            }
            writer = mock.Mock()
            result = launch.poll_once(
                current,
                lambda: launch.reconcile_targets(
                    current,
                    [changed],
                    lambda row: launch.make_room_config(row, lookup_for([row]), rtc_rows()),
                    map_available=lambda _row: False,
                ),
                writer,
            )
            return {
                "contentFailure": (
                    result is current
                    and result[old["room"]].config == current[old["room"]].config
                    and not writer.called
                ) and "preserve" or "changed",
            }

        if name == "heartbeat-retains-full-existing-room":
            existing = room("room.q2.full-scenario.1", "1")
            existing["players"] = existing["createSettings"]["maxPlayers"]
            new = room("room.q2.full-scenario.2", "2")
            new["players"] = new["createSettings"]["maxPlayers"]
            config = launch.make_room_config(existing, lookup_for([existing]), rtc_rows())
            current = {
                existing["room"]: launch.HubTarget(
                    existing["room"], launch._config_text(config), 0, existing["createSettings"]["map"]
                )
            }
            plan = launch.reconcile_targets(
                current,
                [existing, new],
                lambda row: launch.make_room_config(row, lookup_for([row]), rtc_rows()),
            )
            return {
                "fullExisting": "retain" if existing["room"] in plan.targets else "evict",
                "fullNew": "skip" if new["room"] not in plan.targets else "add",
            }

        if name == "heartbeat-invalid-global-rtc":
            selected = room("room.q2.invalid-rtc.1", "1")
            try:
                launch.refresh_targets(
                    {}, "/games/Q2PRO-X.exe",
                    fetch=lambda endpoint: {
                        launch.ROOM_ENDPOINT: catalog([selected]),
                        launch.LOOKUP_ENDPOINT: lookup_for([selected]),
                        launch.RTC_ENDPOINT: payload["rtc"],
                    }[endpoint],
                )
            except launch.LaunchError:
                return {"error": "invalid_global_rtc"}
            return {"error": "accepted"}

        if name in {"heartbeat-duplicate-room-id", "heartbeat-duplicate-derived-alias"}:
            if name.endswith("room-id"):
                rows = [room("room.q2.shared-scenario.1", "1"), room("room.q2.shared-scenario.1", "2")]
            else:
                rows = [room("room.q2.alias-scenario.1", "3"), room("room.q2.alias-scenario.2", "3")]
            try:
                launch.refresh_targets(
                    {}, "/games/Q2PRO-X.exe",
                    fetch=lambda endpoint: {
                        launch.ROOM_ENDPOINT: catalog(rows),
                        launch.LOOKUP_ENDPOINT: lookup_for(rows),
                        launch.RTC_ENDPOINT: rtc_rows(),
                    }[endpoint],
                )
            except launch.LaunchError as exc:
                message = str(exc)
                return {
                    "error": (
                        "duplicate_room_id" if "duplicate room" in message
                        else "duplicate_alias" if "duplicate Q2 aliases" in message
                        else "unexpected"
                    )
                }
            return {"error": "accepted"}

        if name in {"heartbeat-capacity-256-compatible", "heartbeat-capacity-257-compatible"}:
            rows = scenario_rows(payload)
            try:
                plan = launch.make_hub_targets(
                    catalog(rows), lookup_for(rows), rtc_rows(), "/games/Q2PRO-X.exe",
                    content_checker="unused",
                    runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
                )
            except launch.LaunchError as exc:
                return {"error": "catalog_capacity_exceeded", "count": len(rows)}
            return {"count": len(plan)}

        if name == "heartbeat-capacity-replacement":
            current_rows = scenario_rows({"q2": payload["current"]})
            current_targets = launch.make_hub_targets(
                catalog(current_rows), lookup_for(current_rows), rtc_rows(), "/games/Q2PRO-X.exe",
                content_checker="unused",
                runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
            )
            desired_rows = [row for row in current_rows if row["room"] != payload["missing"]]
            desired_rows.append(room(payload["replacement"], payload["replacementOwner"], name="Replacement"))
            lookup = lookup_for(desired_rows)
            rtc = rtc_rows()
            ice_servers = launch._ice_snapshot(rtc)
            make_config = lambda row: launch.make_room_config(row, lookup, rtc, ice_servers)
            first = launch.reconcile_targets(current_targets, desired_rows, make_config)
            second = launch.reconcile_targets(first.targets, desired_rows, make_config)
            return {
                "firstCount": len(first.targets),
                "firstMisses": first.targets[payload["missing"]].misses,
                "firstDrops": len(first.drops),
                "secondCount": len(second.targets),
                "secondDrops": len(second.drops),
                "deferredAdmitted": int(payload["replacement"] in second.targets and payload["replacement"] not in first.targets),
            }

        if name == "heartbeat-ice-snapshot-byte-limit":
            rtc = generated_oversized_rtc(payload)
            self.assertTrue(launch.validate_rtc_payload(rtc))
            try:
                launch._dedicated_ice_servers(rtc)
            except launch.LaunchError as exc:
                return {
                    "error": "ice_snapshot_too_large"
                    if str(exc) == "global ICE snapshot exceeds 65536 bytes" else "unexpected"
                }
            return {"error": "accepted"}

        self.fail(f"unhandled heartbeat scenario {name}")

    def test_shared_catalog_contract_cases(self):
        validators = {
            "rooms": launch.validate_room_payload,
            "aliases": launch.validate_alias_payload,
            "rtc": launch.validate_rtc_payload,
        }
        scenario_names = set()
        for case in CONTRACT_CASES:
            with self.subTest(case=case["name"]):
                if case["kind"] == "heartbeat-scenario":
                    scenario_names.add(case["name"])
                    actual = self._execute_heartbeat_scenario(case)
                    for field, expected in case["expected"].items():
                        self.assertEqual(expected, actual.get(field), f"shared scenario {case['name']} field {field}")
                    continue
                if case["kind"] == "dedicated-ice":
                    try:
                        ice_servers = launch._dedicated_ice_servers(case["payload"])
                    except launch.LaunchError:
                        self.assertFalse(case["valid"])
                    else:
                        self.assertTrue(case["valid"])
                        self.assertEqual(ice_servers, case["expected"]["iceServers"])
                    continue
                self.assertEqual(validators[case["kind"]](case["payload"]), case["valid"])
                if case["kind"] == "rooms":
                    try:
                        selected = launch.joinable_rooms(case["payload"])
                    except launch.LaunchError:
                        selected = []
                    if case["valid"]:
                        self.assertTrue(selected)
                        if "expected" in case:
                            self.assertEqual(launch._room_alias(case["payload"]["rooms"][0]), case["expected"]["alias"])
                    else:
                        self.assertEqual(selected, [])
        self.assertEqual(
            scenario_names,
            {
                "heartbeat-mixed-20-q2-20-q2ded",
                "heartbeat-retains-all-40",
                "heartbeat-map-content-failure-preserves-target",
                "heartbeat-retains-full-existing-room",
                "heartbeat-invalid-global-rtc",
                "heartbeat-duplicate-room-id",
                "heartbeat-duplicate-derived-alias",
                "heartbeat-capacity-256-compatible",
                "heartbeat-capacity-257-compatible",
                "heartbeat-capacity-replacement",
                "heartbeat-ice-snapshot-byte-limit",
            },
        )

    def test_mixed_20_q2_and_20_dedicated_have_stable_40_target_order(self):
        rows = scenario_rows({"q2": 20, "q2ded": 20})
        targets = launch.make_hub_targets(
            catalog(rows), lookup_for(rows), rtc_rows(), "/games/Q2PRO-X.exe",
            content_checker="unused",
            runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        self.assertEqual(len(targets), 40)
        expected = sorted(row["room"] for row in rows)
        self.assertEqual(list(targets), expected)

    def test_successful_heartbeat_retains_all_40_and_puts_in_room_id_order(self):
        rows = scenario_rows({"q2": 20, "q2ded": 20})
        plan = launch.refresh_targets(
            {}, "/games/Q2PRO-X.exe",
            fetch=lambda endpoint: {
                launch.ROOM_ENDPOINT: catalog(rows),
                launch.LOOKUP_ENDPOINT: lookup_for(rows),
                launch.RTC_ENDPOINT: rtc_rows(),
            }[endpoint],
            content_checker="unused",
            runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        process = FakeProcess()
        launch.write_heartbeat(process, plan, generation=1)
        lines = decoded_lines(process)
        self.assertEqual(len([line for line in lines if " PUT " in line]), 40)
        self.assertEqual(
            [line.split(" room=", 1)[1].split(" json=", 1)[0] for line in lines if " PUT " in line],
            sorted(targets for targets in plan.targets),
        )
        self.assertEqual(sum(" ICE " in line for line in lines), 1)

    def test_one_successful_miss_retains_then_second_miss_drops_room(self):
        first = launch.HubTarget("room.q2.keep.1", json.dumps({"roomId": "room.q2.keep.1"}), map_name="q2dm1")
        current = {first.room_id: first}
        one_miss = launch.reconcile_targets(current, [], lambda _row: "unused")
        self.assertIn(first.room_id, one_miss.targets)
        self.assertEqual(one_miss.targets[first.room_id].misses, 1)
        self.assertEqual(one_miss.drops, ())
        two_misses = launch.reconcile_targets(one_miss.targets, [], lambda _row: "unused")
        self.assertNotIn(first.room_id, two_misses.targets)
        self.assertEqual(two_misses.drops, (first.room_id,))

    def test_map_content_failure_preserves_prior_working_target_and_emits_nothing(self):
        old = launch.HubTarget(
            "room.q2.map.1",
            json.dumps({"roomId": "room.q2.map.1", "map": "q2dm1"}, separators=(",", ":")),
            map_name="q2dm1",
        )
        changed = room(old.room_id, map_name="missing")
        writer = mock.Mock()
        result = launch.poll_once(
            {old.room_id: old},
            lambda: launch.reconcile_targets(
                {old.room_id: old}, [changed],
                lambda row: {"roomId": row["room"], "map": row["createSettings"]["map"]},
                map_available=lambda _row: False,
            ),
            writer,
        )
        self.assertIn(old.room_id, result)
        writer.assert_not_called()
        self.assertEqual(result[old.room_id].config, old.config)

    def test_full_existing_room_is_retained_but_new_full_room_is_not_added(self):
        existing = room("room.q2.existing.1", "1")
        target = launch.HubTarget(
            existing["room"], launch._config_text({"roomId": existing["room"], "map": "q2dm1"}), map_name="q2dm1"
        )
        full_existing = dict(existing)
        full_existing["players"] = full_existing["createSettings"]["maxPlayers"]
        full_new = room("room.q2.new-full.2", "2")
        full_new["players"] = full_new["createSettings"]["maxPlayers"]
        plan = launch.reconcile_targets(
            {target.room_id: target}, [full_existing, full_new],
            lambda row: {"roomId": row["room"], "map": row["createSettings"]["map"]},
        )
        self.assertIn(target.room_id, plan.targets)
        self.assertNotIn(full_new["room"], plan.targets)

    def test_display_only_room_change_does_not_restart_transport(self):
        old = room("room.q2.display.1", "1", name="Old")
        old_config = launch.make_room_config(old, lookup_for([old]), rtc_rows())
        changed = room(old["room"], "1", name="New")
        plan = launch.reconcile_targets(
            {old["room"]: launch.HubTarget(old["room"], launch._config_text(old_config), map_name="q2dm1")},
            [changed],
            lambda row: launch.make_room_config(row, lookup_for([row]), rtc_rows()),
        )
        self.assertEqual(plan.drops, ())

    def test_changed_transport_emits_drop_before_put(self):
        old = room("room.q2.transport.1", "1", map_name="q2dm1")
        old_config = launch.make_room_config(old, lookup_for([old]), rtc_rows())
        changed = room(old["room"], "2", map_name="q2dm2")
        new_config = launch.make_room_config(changed, lookup_for([changed]), rtc_rows())
        plan = launch.reconcile_targets(
            {old["room"]: launch.HubTarget(old["room"], launch._config_text(old_config), map_name="q2dm1")},
            [changed], lambda _row: new_config,
            map_available=lambda _row: True,
        )
        process = FakeProcess()
        launch.write_heartbeat(process, plan, generation=2)
        lines = decoded_lines(process)
        drop = next(index for index, line in enumerate(lines) if " DROP room=" in line)
        put = next(index for index, line in enumerate(lines) if " PUT room=" in line)
        self.assertLess(drop, put)

    def test_duplicate_room_and_derived_alias_are_rejected(self):
        duplicate_id = room("room.q2.duplicate.1", "1")
        duplicate_id_again = room("room.q2.duplicate.1", "2")
        duplicate_alias_a = room("room.q2.alias-a.1", "3")
        duplicate_alias_b = room("room.q2.alias-b.2", "3")
        with self.assertRaisesRegex(launch.LaunchError, "duplicate room"):
            launch.refresh_targets(
                {}, "/games/Q2PRO-X.exe",
                fetch=lambda endpoint: {
                    launch.ROOM_ENDPOINT: catalog([duplicate_id, duplicate_id_again]),
                    launch.LOOKUP_ENDPOINT: {"aliases": ["q2.1", "q2.2"]},
                    launch.RTC_ENDPOINT: rtc_rows(),
                }[endpoint],
            )
        with self.assertRaisesRegex(launch.LaunchError, "duplicate Q2 aliases"):
            launch.refresh_targets(
                {}, "/games/Q2PRO-X.exe",
                fetch=lambda endpoint: {
                    launch.ROOM_ENDPOINT: catalog([duplicate_alias_a, duplicate_alias_b]),
                    launch.LOOKUP_ENDPOINT: {"aliases": ["q2.3"]},
                    launch.RTC_ENDPOINT: rtc_rows(),
                }[endpoint],
            )

    def test_invalid_global_rtc_preserves_state_and_writes_no_commands(self):
        old = room("room.q2.rtc.1", "1")
        config = launch.make_room_config(old, lookup_for([old]), rtc_rows())
        current = {old["room"]: launch.HubTarget(old["room"], launch._config_text(config), map_name="q2dm1")}
        writer = mock.Mock()
        result = launch.poll_once(
            current,
            lambda: launch.refresh_targets(
                current, "/games/Q2PRO-X.exe",
                fetch=lambda endpoint: {
                    launch.ROOM_ENDPOINT: catalog([old]),
                    launch.LOOKUP_ENDPOINT: lookup_for([old]),
                    launch.RTC_ENDPOINT: [{"domain": "bad domain", "iceservers": [{"urls": ["stun:example.invalid"]}]}],
                }[endpoint],
            ),
            writer,
        )
        self.assertIs(result, current)
        writer.assert_not_called()

    def test_exact_256_is_retained_and_257_fails_without_truncation(self):
        rows = scenario_rows({"q2": 256})
        targets = launch.make_hub_targets(
            catalog(rows), lookup_for(rows), rtc_rows(), "/games/Q2PRO-X.exe",
            content_checker="unused",
            runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        self.assertEqual(len(targets), 256)
        self.assertEqual(list(targets), sorted(row["room"] for row in rows))
        too_many = scenario_rows({"q2": 257})
        with self.assertRaisesRegex(launch.LaunchError, "catalog_capacity_exceeded"):
            launch.make_hub_targets(
                catalog(too_many), lookup_for(too_many), rtc_rows(), "/games/Q2PRO-X.exe",
                content_checker="unused",
                runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
            )

    def test_refresh_fetches_and_validates_one_global_rtc_snapshot(self):
        selected = room("room.q2.a.1", "1")
        endpoints = []
        replies = {
            launch.ROOM_ENDPOINT: catalog([selected]),
            launch.LOOKUP_ENDPOINT: lookup_for([selected]),
            launch.RTC_ENDPOINT: rtc_rows(),
        }
        plan = launch.refresh_targets(
            {}, "/games/Q2PRO-X.exe", fetch=lambda endpoint: endpoints.append(endpoint) or replies[endpoint],
            content_checker="unused", runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        self.assertEqual(endpoints, [launch.ROOM_ENDPOINT, launch.LOOKUP_ENDPOINT, launch.RTC_ENDPOINT])
        self.assertEqual(len(plan.targets), 1)
        self.assertEqual(plan.ice_servers, rtc_rows()[0]["iceservers"])

    def test_protocol_commands_are_utf8_and_keep_credentials_out_of_argv(self):
        config = {
            "version": 1,
            "roomId": "room.q2.abc.123",
            "alias": "q2.123",
            "signalingUrl": launch.SIGNALING_URL,
            "ordered": False,
            "iceServers": [{"urls": ["turn:example.invalid"], "username": "u", "credential": "secret"}],
            "map": "q2dm1",
            "game": launch.GAME,
        }
        target = launch.HubTarget(config["roomId"], launch._config_text(config), map_name="q2dm1")
        process = FakeProcess()
        launch.write_heartbeat(process, launch.TargetPlan({target.room_id: target}, [config["iceServers"][0]], ()), generation=1)
        text = process.stdin.payload.decode("utf-8")
        self.assertIn('"credential":"secret"', text)
        source = (ROOT / "tools" / "q2dz-launch").read_text(encoding="utf-8")
        self.assertIn("payload_bytes = bytearray()", source)
        self.assertIn("payload_bytes[:] = b\"\\x00\" * len(payload_bytes)", source)
        self.assertIn("encoded[:] = b\"\\x00\" * len(encoded)", source)
        self.assertIn("lines.clear()", source)

    def test_credentials_stay_in_pipe_and_never_reach_argv_or_files(self):
        secret = "turn-secret-regression"
        config = {
            "version": 1,
            "roomId": "room.q2.secret.1",
            "alias": "q2.1",
            "signalingUrl": launch.SIGNALING_URL,
            "ordered": False,
            "iceServers": [{"urls": ["turn:example.invalid"], "username": "u", "credential": secret}],
            "map": "q2dm1",
            "game": launch.GAME,
        }
        target = launch.HubTarget(config["roomId"], launch._config_text(config), map_name="q2dm1")
        plan = launch.TargetPlan({target.room_id: target}, config["iceServers"], ())
        started = {}

        def starter(args, **kwargs):
            started["args"] = args
            started["kwargs"] = kwargs
            return FakeProcess()

        with tempfile.TemporaryDirectory() as tmp:
            result = launch.run_hub(
                plan,
                "/games/Q2PRO-X.exe",
                session=Path(tmp) / "session",
                managed=Path(tmp) / "managed",
                legacy=Path(tmp) / "legacy",
                adapter=Path(tmp) / "adapter",
                popen=starter,
                refresh=lambda _targets, _q2_path: plan,
                poll_interval=0,
            )
            self.assertEqual(result, 0)
            self.assertFalse(any(secret in str(argument) for argument in started["args"]))
            self.assertNotIn(secret, str(started["kwargs"].get("env", {})))
            self.assertIs(started["kwargs"]["stdin"], subprocess.PIPE)
            self.assertEqual(list(Path(tmp).rglob("*")), [])

    def test_main_passes_initial_target_plan_with_fresh_ice_to_hub(self):
        plan = launch.TargetPlan({}, rtc_rows()[0]["iceservers"])
        with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
             mock.patch.object(launch, "_installed_paths", return_value={
                 "session": Path("session"), "managed": Path("managed"),
                 "legacy": Path("legacy"), "adapter": Path("adapter"),
             }), \
             mock.patch.object(launch, "refresh_targets", return_value=plan), \
             mock.patch.object(launch, "run_hub", return_value=0) as run_hub:
            self.assertEqual(launch.main([]), 0)
        self.assertIs(run_hub.call_args.args[0], plan)

    def test_initial_refresh_failure_enters_hub_empty_and_recovers_without_initial_command(self):
        recovered = room("room.q2.recovered.1", "1")
        recovered_plan = launch.refresh_targets(
            {}, "/games/Q2PRO-X.exe",
            fetch=lambda endpoint: {
                launch.ROOM_ENDPOINT: catalog([recovered]),
                launch.LOOKUP_ENDPOINT: lookup_for([recovered]),
                launch.RTC_ENDPOINT: rtc_rows(),
            }[endpoint],
            content_checker="unused",
            runner=lambda *args, **kwargs: subprocess.CompletedProcess(args, 0),
        )
        process = FakeProcess(waits_to_exit=2)
        seen_targets = []
        result = launch.run_hub(
            launch.TargetPlan({}, []),
            "/games/Q2PRO-X.exe",
            session=Path("session"),
            managed=Path("managed"),
            legacy=Path("legacy"),
            adapter=Path("adapter"),
            popen=lambda _args, **kwargs: process,
            refresh=lambda current, _q2_path: seen_targets.append(current) or recovered_plan,
            poll_interval=0,
        )
        self.assertEqual(result, 0)
        self.assertEqual(seen_targets, [{}])
        lines = decoded_lines(process)
        self.assertEqual(len(lines), 2)
        self.assertIn(" ICE ", lines[0])
        self.assertIn(" PUT room=room.q2.recovered.1 ", lines[1])

        for failure in ("network unavailable", "room catalog has an invalid schema", "map content check failed"):
            with self.subTest(initial_failure=failure):
                captured = {}

                def fake_run_hub(initial, *_args, **_kwargs):
                    captured["initial"] = initial
                    return 0

                with mock.patch.object(launch, "discover_q2", return_value=Path("/games/Q2PRO-X.exe")), \
                     mock.patch.object(launch, "_installed_paths", return_value={
                         "session": Path("session"), "managed": Path("managed"),
                         "legacy": Path("legacy"), "adapter": Path("adapter"),
                     }), \
                     mock.patch.object(launch, "refresh_targets", side_effect=launch.LaunchError(failure)), \
                     mock.patch.object(launch, "run_hub", side_effect=fake_run_hub):
                    self.assertEqual(launch.main([]), 0)
                self.assertIsInstance(captured["initial"], launch.TargetPlan)
                self.assertEqual(captured["initial"].targets, {})
                self.assertEqual(captured["initial"].ice_servers, [])

    def test_fetch_json_accepts_bounded_gzip_and_rejects_bad_input(self):
        body = gzip.compress(json.dumps(catalog([room()])).encode())
        result = launch.fetch_json(
            launch.ROOM_ENDPOINT, opener=lambda _request, timeout: Response(body, "gzip")
        )
        self.assertEqual(result["rooms"][0]["room"], room()["room"])
        with self.assertRaises(launch.LaunchError):
            launch.fetch_json(launch.ROOM_ENDPOINT, opener=lambda _request, timeout: Response(b"{bad"))
        for status in (302, 500):
            with self.subTest(status=status), self.assertRaises(launch.LaunchError):
                launch.fetch_json(
                    launch.ROOM_ENDPOINT,
                    opener=lambda _request, timeout, status=status: Response(b"{}", status=status),
                )

    def test_help_and_select_do_not_start_discovery_or_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [os.sys.executable, str(ROOT / "tools" / "q2dz-launch"), "--help"],
                input="", capture_output=True, text=True, check=False,
                env={"HOME": tmp, "XDG_CONFIG_HOME": tmp, "PATH": "/nonexistent"},
            )
        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage:", result.stdout)
        with mock.patch.object(launch, "select_q2_path", return_value=Path("/games/Q2PRO-X.exe")) as selected, \
             mock.patch.object(launch, "fetch_json") as fetched:
            self.assertEqual(launch.main(["--select-q2"]), 0)
        selected.assert_called_once_with()
        fetched.assert_not_called()

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

    def test_missing_client_offers_download_without_file_picker(self):
        calls = []

        def fake_run(args, **_kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(args, 0, stdout="Open download page\n")

        with mock.patch.object(launch.webbrowser, "open") as opened:
            self.assertIsNone(launch.select_q2_path(zenity="/usr/bin/zenity", runner=fake_run))
        self.assertIn("--question", calls[0])
        opened.assert_called_once_with(launch.OFFICIAL_URL)


if __name__ == "__main__":
    unittest.main()
