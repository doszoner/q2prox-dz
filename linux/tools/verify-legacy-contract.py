#!/usr/bin/env python3
"""Fail-closed check that the configured legacy gateway accepts required options.

This never passes room secrets. It checks the adapter statically and executes the
gateway with a bounded sealed inherited ICE memfd, proving that legacy arguments
reach transport initialization without requiring a live peer.
"""
from __future__ import annotations
import argparse
import fcntl
import json
import os
import pathlib
import re
import subprocess
import sys

REQUIRED_PLACEHOLDERS = {"{alias}", "{ice_memfd}", "{internal_endpoint}"}
FORBIDDEN_PLACEHOLDERS = {"{ice_json}", "{credential}", "{token}", "{secret}"}
REQUIRED_OPTIONS = {"--bind", "--signal", "--ice-file", "--alias", "--max-datagram"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway", required=True)
    ap.add_argument("--adapter", required=True)
    ap.add_argument("--source")
    ns = ap.parse_args()

    gateway = pathlib.Path(ns.gateway).resolve()
    adapter_path = pathlib.Path(ns.adapter).resolve()
    if not gateway.is_file() or not gateway.stat().st_mode & 0o111:
        raise SystemExit(f"not an executable gateway: {gateway}")
    cfg = json.loads(adapter_path.read_text(encoding="utf-8"))
    argv = cfg.get("argv")
    if not isinstance(argv, list) or not all(isinstance(v, str) for v in argv):
        raise SystemExit("adapter argv must be a string array")
    joined = "\n".join(argv)
    missing = sorted(p for p in REQUIRED_PLACEHOLDERS if p not in joined)
    forbidden = sorted(p for p in FORBIDDEN_PLACEHOLDERS if p in joined)
    if missing or forbidden:
        raise SystemExit(f"invalid adapter placeholders: missing={missing}, forbidden={forbidden}")

    option_names = sorted(set(re.findall(r"--[a-zA-Z0-9][a-zA-Z0-9-]*", joined)))
    if set(option_names) != REQUIRED_OPTIONS or "wss://net.js-dos.com:444" not in argv:
        raise SystemExit(f"adapter does not match the proven gateway contract: {option_names}")
    if ns.source:
        source = pathlib.Path(ns.source).resolve()
        if not source.is_file():
            raise SystemExit(f"source not found: {source}")
        evidence = source.read_text(encoding="utf-8", errors="replace")
        absent = [name for name in option_names if name not in evidence]
        if absent:
            raise SystemExit(f"legacy gateway contract is not proven; missing options: {absent}")

    def run_gateway(extra_args: list[str]) -> subprocess.CompletedProcess:
        try:
            ice_fd = os.memfd_create(
                "q2dz-legacy-contract", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING
            )
            if os.write(ice_fd, b"[{}]") != 4:
                raise OSError("short ICE memfd write")
            os.lseek(ice_fd, 0, os.SEEK_SET)
            required_seals = (
                fcntl.F_SEAL_SEAL
                | fcntl.F_SEAL_SHRINK
                | fcntl.F_SEAL_GROW
                | fcntl.F_SEAL_WRITE
            )
            fcntl.fcntl(ice_fd, fcntl.F_ADD_SEALS, required_seals)
            arguments = [
                str(gateway), *extra_args, "--bind", "127.0.0.1:27999",
                "--signal", "wss://net.js-dos.com:444",
                "--ice-file", f"/proc/self/fd/{ice_fd}",
                "--alias", "q2.contract-test", "--max-datagram", "1400",
            ]
            try:
                return subprocess.run(
                    arguments, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=5, check=False,
                    env={"PATH": "/usr/bin:/bin"}, pass_fds=(ice_fd,),
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise SystemExit(f"unable to execute legacy gateway contract: {exc}")
        except OSError as exc:
            raise SystemExit(f"unable to create sealed ICE memfd: {exc}")
        finally:
            if "ice_fd" in locals():
                os.close(ice_fd)

    cp = run_gateway([])
    # The intended bounded failure is the sealed-memfd ICE/transport-configure
    # step inside run_legacy_gateway, which gets its own fixed category
    # (transport_init_failed) distinct from the generic "startup_failed"
    # default and from any CLI-argument error (round-4 defect: all three
    # used to collapse onto "startup_failed").
    expected_output = "GATEWAY_ERROR category=transport_init_failed"
    if cp.returncode != 2 or cp.stdout.strip() != expected_output:
        raise SystemExit(
            "legacy gateway execution did not reach the expected bounded validation "
            f"path: returncode={cp.returncode}, output={cp.stdout!r}"
        )

    # Negative control (round-4 defect): an unknown option must NOT satisfy
    # the same success predicate as the intended bounded init failure above
    # — it has to land in its own category, not transport_init_failed's.
    canary = run_gateway(["--bogus-option-CANARY123"])
    if canary.returncode == 2 and canary.stdout.strip() == expected_output:
        raise SystemExit(
            "legacy gateway contract is not discriminating: an unknown option "
            f"satisfied the transport_init_failed predicate: output={canary.stdout!r}"
        )

    print(json.dumps({
        "status": "PASS",
        "gateway": str(gateway),
        "adapter": str(adapter_path),
        "verified_options": option_names,
        "legacy_execution": {"status": "PASS", "returncode": cp.returncode},
    }, indent=2))
    return 0

if __name__ == "__main__":
    sys.exit(main())
