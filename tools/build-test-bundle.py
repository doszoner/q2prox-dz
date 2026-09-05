#!/usr/bin/env python3
"""Build the reviewer/tester bundle from canonical release artifacts."""

from __future__ import annotations

import hashlib
import pathlib
import shutil
import subprocess
import tempfile
import zipfile


REPO = pathlib.Path(__file__).resolve().parents[1]
VERSION = (REPO / "VERSION").read_text(encoding="utf-8").strip()
OUT = REPO / "dist" / f"q2prox-dz-{VERSION}-test-bundle.zip"
ZIP_TIME = (1980, 1, 1, 0, 0, 0)


FILES = {
    f"{VERSION}.md": REPO / f"docs/verification/{VERSION}.md",
    "2026-09-05-exact-q2pro-x-40-source-attempts.md": REPO
    / "docs/verification/2026-09-05-exact-q2pro-x-40-source-attempts.md",
    "2026-09-05-exact-q2pro-x-40-source-runbook.md": REPO
    / "docs/verification/2026-09-05-exact-q2pro-x-40-source-runbook.md",
    "2026-09-05-live-gameplay-gates-runbook.md": REPO
    / "docs/verification/2026-09-05-live-gameplay-gates-runbook.md",
    "2026-09-05-windows-headless-interactive-gate-preflight.md": REPO
    / "docs/verification/2026-09-05-windows-headless-interactive-gate-preflight.md",
    "2026-09-05-remaining-release-gates.md": REPO
    / "docs/verification/2026-09-05-remaining-release-gates.md",
    "CHANGELOG.md": REPO / "CHANGELOG.md",
    "INSTALLATION.md": REPO / "INSTALLATION.md",
    "README.md": REPO / "README.md",
    "docs/RELEASE.md": REPO / "docs/RELEASE.md",
    "tests/tools/controlled-room-set.py": REPO / "tests/tools/controlled-room-set.py",
    f"q2prox-dz-{VERSION}-linux-x86_64.tar.gz": REPO
    / "linux/dist"
    / f"q2prox-dz-{VERSION}-linux-x86_64.tar.gz",
    f"q2prox-dz-{VERSION}-linux-x86_64.tar.gz.sha256": REPO
    / "linux/dist"
    / f"q2prox-dz-{VERSION}-linux-x86_64.tar.gz.sha256",
    f"q2prox-dz-{VERSION}-windows-x64-portable.zip": REPO
    / "dist"
    / f"q2prox-dz-{VERSION}-windows-x64-portable.zip",
    f"q2prox-dz-{VERSION}-windows-x64-portable.zip.sha256": REPO
    / "dist"
    / f"q2prox-dz-{VERSION}-windows-x64-portable.zip.sha256",
    f"q2prox-dz-{VERSION}-windows-x64-setup.zip": REPO
    / "dist"
    / f"q2prox-dz-{VERSION}-windows-x64-setup.zip",
    f"q2prox-dz-{VERSION}-windows-x64-setup.zip.sha256": REPO
    / "dist"
    / f"q2prox-dz-{VERSION}-windows-x64-setup.zip.sha256",
}


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="q2dz-test-bundle-") as temporary:
        root = pathlib.Path(temporary)
        for member, source in FILES.items():
            if not source.is_file():
                raise SystemExit(f"missing bundle input: {source}")
            target = root / member
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

        sums = []
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.name != "SHA256SUMS.txt":
                sums.append(f"{sha256(path)}  {path.relative_to(root).as_posix()}\n")
        (root / "SHA256SUMS.txt").write_text("".join(sums), encoding="utf-8")

        OUT.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(OUT, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(root.rglob("*")):
                if not path.is_file():
                    continue
                name = path.relative_to(root).as_posix()
                info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, path.read_bytes())

    sidecar = pathlib.Path(str(OUT) + ".sha256")
    sidecar.write_text(f"{sha256(OUT)}  {OUT.name}\n", encoding="utf-8")
    subprocess.run(["sha256sum", "-c", sidecar.name], cwd=OUT.parent, check=True)
    with tempfile.TemporaryDirectory(prefix="q2dz-test-bundle-verify-") as temporary:
        with zipfile.ZipFile(OUT) as archive:
            archive.extractall(temporary)
        subprocess.run(["sha256sum", "-c", "SHA256SUMS.txt"], cwd=temporary, check=True)
    print(sidecar.read_text(encoding="utf-8"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
