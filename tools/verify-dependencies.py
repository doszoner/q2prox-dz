#!/usr/bin/env python3
"""Verify pinned WebRTC-NET and WebView2 inputs without external packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class VerificationError(Exception):
    pass


def git(source: Path, *args: str) -> str:
    # Windows release inputs may be an exact read-only VirtualBox/SMB checkout.
    # Allow only this checkout path; never broaden Git's global trust policy.
    safe_source = source.resolve().as_posix()
    try:
        result = subprocess.run(
            ["git", "-c", f"safe.directory={safe_source}", "-C", str(source), *args],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise VerificationError(f"git command failed for {source}") from exc
    return result.stdout.strip()


def digest(path: Path) -> str:
    try:
        with path.open("rb") as stream:
            hashed = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                hashed.update(block)
            return hashed.hexdigest()
    except OSError as exc:
        raise VerificationError(f"cannot read artifact: {path}") from exc


def expected_hash(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise VerificationError(f"invalid SHA-256 lock value: {label}")
    try:
        int(value, 16)
    except ValueError as exc:
        raise VerificationError(f"invalid SHA-256 lock value: {label}") from exc
    return value.lower()


def relative_path(root: Path, path: Path) -> str | None:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def verify_artifacts(root: Path, entries: object, label: str, allowed_extra: set[str] | None = None) -> None:
    if not root.is_dir():
        raise VerificationError(f"release root is missing: {root}")
    if not isinstance(entries, dict) or not entries:
        raise VerificationError(f"no locked artifacts for {label}")
    allowed = set(allowed_extra or ())
    for relative, locked in entries.items():
        if not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise VerificationError(f"invalid artifact path in lock: {label}/{relative}")
        allowed.add(relative)
        path = root / relative
        if not path.is_file():
            raise VerificationError(f"missing artifact: {path}")
        actual = digest(path)
        wanted = expected_hash(locked, f"{label}/{relative}")
        if actual != wanted:
            raise VerificationError(f"hash mismatch: {label}/{relative}")
    for path in root.rglob("*"):
        if path.is_file() and path.relative_to(root).as_posix() not in allowed:
            raise VerificationError(f"unlisted artifact: {path}")


def verify_source(source: Path, locked: dict) -> None:
    if not source.is_dir():
        raise VerificationError(f"source root is missing: {source}")
    wanted_commit = locked.get("commit")
    if not isinstance(wanted_commit, str):
        raise VerificationError("source lock must contain commit")
    if git(source, "rev-parse", "HEAD") != wanted_commit:
        raise VerificationError("WebRTC-NET commit mismatch")

    submodules = locked.get("submodules", {})
    if not isinstance(submodules, dict):
        raise VerificationError("source lock submodules must be an object")
    for relative, wanted in submodules.items():
        if not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise VerificationError(f"invalid submodule path in lock: {relative}")
        if not isinstance(wanted, str):
            raise VerificationError(f"invalid submodule commit in lock: {relative}")
        submodule = source / relative
        if not submodule.is_dir():
            raise VerificationError(f"missing submodule: {relative}")
        if git(submodule, "rev-parse", "HEAD") != wanted:
            raise VerificationError(f"submodule commit mismatch: {relative}")

    license_sources = locked.get("licenseSources")
    if not isinstance(license_sources, list) or not license_sources:
        raise VerificationError("source lock licenseSources must be a non-empty array")
    for relative in license_sources:
        if not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise VerificationError("invalid WebRTC-NET license source")
        path = source / relative
        if path.is_symlink() or not path.is_file() or relative_path(source, path) is None:
            raise VerificationError("invalid WebRTC-NET license source")

        repository, commit, tracked = source, wanted_commit, relative
        for submodule_relative, submodule_commit in submodules.items():
            try:
                tracked = Path(relative).relative_to(submodule_relative).as_posix()
            except ValueError:
                continue
            repository, commit = source / submodule_relative, submodule_commit
            break
        try:
            git(repository, "cat-file", "-e", f"{commit}:{tracked}")
        except VerificationError as exc:
            raise VerificationError("invalid WebRTC-NET license source") from exc
        if git(repository, "diff", "--name-only", commit, "--", tracked) or git(
            repository, "diff", "--cached", "--name-only", commit, "--", tracked
        ):
            raise VerificationError("WebRTC-NET license source mismatch")


def verify_webview2_package(path: Path, locked: dict) -> None:
    if not path.is_file():
        raise VerificationError(f"WebView2 package is missing: {path}")
    actual = digest(path)
    wanted = expected_hash(locked.get("packageSha256"), "webView2/package")
    if actual != wanted:
        raise VerificationError("WebView2 package hash mismatch")


def loader_candidates(root: Path) -> list[Path]:
    return [
        root / "WebView2Loader.dll",
        root / "gateway" / "WebView2Loader.dll",
        root / "build" / "native" / "x64" / "WebView2Loader.dll",
        root / "runtimes" / "win-x64" / "native" / "WebView2Loader.dll",
    ]


def verify_webview2_loader(path: Path, locked: dict) -> None:
    if not path.is_file():
        raise VerificationError(f"WebView2 loader is missing: {path}")
    actual = digest(path)
    wanted = expected_hash(locked.get("loaderX64Sha256"), "webView2/loaderX64")
    if actual != wanted:
        raise VerificationError("WebView2 loader hash mismatch")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify locked WebRTC-NET/WebView2 inputs.")
    parser.add_argument("--lock", type=Path, default=ROOT / "deps.lock.json")
    parser.add_argument("--webrtc-source", type=Path, help="WebRTC-NET source checkout")
    parser.add_argument("--linux-release", "--linux-release-root", "--linux-root", dest="linux_release", type=Path)
    parser.add_argument("--windows-release", "--windows-release-root", "--windows-root", dest="windows_release", type=Path)
    parser.add_argument("--webview2-package", type=Path, help="WebView2 NuGet package")
    parser.add_argument("--webview2-loader", type=Path, help="WebView2 x64 loader DLL")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not any((args.webrtc_source, args.linux_release, args.windows_release, args.webview2_package, args.webview2_loader)):
        raise VerificationError("at least one explicit dependency input is required")
    try:
        with args.lock.open(encoding="utf-8") as stream:
            lock = json.load(stream)
        if not isinstance(lock, dict) or lock.get("schema") != 1 or not isinstance(lock.get("webrtcNet"), dict) or not isinstance(lock.get("webView2"), dict):
            raise VerificationError("unsupported dependency lock schema")
        webrtc = lock["webrtcNet"]
        webview2 = lock["webView2"]
        if args.webrtc_source:
            verify_source(args.webrtc_source, webrtc)
        if args.linux_release:
            verify_artifacts(args.linux_release, webrtc.get("linux"), "webrtcNet/linux")
        if args.windows_release:
            loader = args.webview2_loader
            if loader is None:
                loader = next((candidate for candidate in loader_candidates(args.windows_release) if candidate.is_file()), None)
            if loader is None:
                raise VerificationError("WebView2 x64 loader is missing from Windows release root")
            loader_relative = relative_path(args.windows_release, loader)
            allowed_extra = {loader_relative} if loader_relative is not None and loader.is_file() else set()
            verify_artifacts(args.windows_release, webrtc.get("windows"), "webrtcNet/windows", allowed_extra)
            verify_webview2_loader(loader, webview2)
        if args.webview2_package:
            verify_webview2_package(args.webview2_package, webview2)
        if args.webview2_loader and not args.windows_release:
            verify_webview2_loader(args.webview2_loader, webview2)
    except (OSError, json.JSONDecodeError, VerificationError) as exc:
        print(f"dependency verification failed: {exc}", file=sys.stderr)
        return 1
    print("dependency verification: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
