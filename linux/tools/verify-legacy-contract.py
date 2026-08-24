#!/usr/bin/env python3
"""Fail-closed check that the configured legacy gateway accepts required options.

This never passes room secrets. It checks only static option names against --help
and, optionally, source text. Unknown or ambiguous contracts fail.
"""
from __future__ import annotations
import argparse
import json
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
    try:
        cp = subprocess.run(
            [str(gateway), "--help"], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=5, check=False,
            env={"PATH": "/usr/bin:/bin"},
        )
        evidence = cp.stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SystemExit(f"unable to obtain gateway --help: {exc}")

    if ns.source:
        source = pathlib.Path(ns.source).resolve()
        if not source.is_file():
            raise SystemExit(f"source not found: {source}")
        evidence += "\n" + source.read_text(encoding="utf-8", errors="replace")

    absent = [name for name in option_names if name not in evidence]
    if absent:
        raise SystemExit(f"legacy gateway contract is not proven; missing options: {absent}")
    print(json.dumps({
        "status": "PASS",
        "gateway": str(gateway),
        "adapter": str(adapter_path),
        "verified_options": option_names,
    }, indent=2))
    return 0

if __name__ == "__main__":
    sys.exit(main())
