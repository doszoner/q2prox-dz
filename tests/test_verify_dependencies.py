#!/usr/bin/env python3
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "tools" / "verify-dependencies.py"


class DependencyVerifierTests(unittest.TestCase):
    def run_verifier(self, lock, release, source=None):
        args = [
            sys.executable,
            str(VERIFIER),
            "--lock",
            str(lock),
            "--linux-release",
            str(release),
        ]
        if source is not None:
            args.extend(["--webrtc-source", str(source)])
        return subprocess.run(args, capture_output=True, text=True, check=False)

    def make_tree(self):
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        release = root / "release"
        release.mkdir()
        artifact = release / "libhumblenet.so"
        artifact.write_bytes(b"fixture")

        source = root / "source"
        source.mkdir()
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "config", "user.name", "test"], check=True)
        subprocess.run(["git", "-C", str(source), "config", "user.email", "test@example.invalid"], check=True)
        (source / "marker").write_text("fixture\n", encoding="utf-8")
        (source / "LICENSE.md").write_text("locked license\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(source), "add", "marker", "LICENSE.md"], check=True)
        subprocess.run(["git", "-C", str(source), "commit", "-qm", "fixture"], check=True)
        subprocess.run(["git", "-C", str(source), "tag", "v1.0.0"], check=True)
        commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        lock = root / "deps.lock.json"
        lock.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "webrtcNet": {
                        "source": "https://example.invalid/WebRTC-NET.git",
                        "tag": "v1.0.0",
                        "commit": commit,
                        "submodules": {},
                        "licenseSources": ["LICENSE.md"],
                        "linux": {"libhumblenet.so": digest},
                        "windows": {},
                    },
                    "webView2": {
                        "package": "Microsoft.Web.WebView2",
                        "version": "1.0.0",
                        "source": "https://example.invalid/webview2.nupkg",
                        "packageSha256": "0" * 64,
                        "loaderX64Sha256": "0" * 64,
                        "licenseSources": [],
                    },
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        return temp, lock, release, source

    def test_valid_tree_passes(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        result = self.run_verifier(lock, release, source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_tagless_source_at_locked_commit_passes(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        data = json.loads(lock.read_text(encoding="utf-8"))
        del data["webrtcNet"]["tag"]
        lock.write_text(json.dumps(data), encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(source), "tag", "-d", "v1.0.0"],
            check=True,
            capture_output=True,
        )
        result = self.run_verifier(lock, release, source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_artifact_fails_closed(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        (release / "libhumblenet.so").unlink()
        self.assertNotEqual(self.run_verifier(lock, release, source).returncode, 0)

    def test_hash_mismatch_fails_closed(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        (release / "libhumblenet.so").write_bytes(b"altered")
        self.assertNotEqual(self.run_verifier(lock, release, source).returncode, 0)

    def test_unlisted_regular_file_fails_closed(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        (release / "extra.bin").write_bytes(b"unlisted")
        self.assertNotEqual(self.run_verifier(lock, release, source).returncode, 0)

    def test_source_commit_mismatch_fails_closed(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        data = json.loads(lock.read_text(encoding="utf-8"))
        data["webrtcNet"]["commit"] = "0" * 40
        lock.write_text(json.dumps(data), encoding="utf-8")
        self.assertNotEqual(self.run_verifier(lock, release, source).returncode, 0)

    def test_modified_license_source_fails_closed(self):
        temp, lock, release, source = self.make_tree()
        self.addCleanup(temp.cleanup)
        (source / "LICENSE.md").write_text("modified license\n", encoding="utf-8")
        result = self.run_verifier(lock, release, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("WebRTC-NET license source mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
