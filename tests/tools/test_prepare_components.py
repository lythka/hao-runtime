"""Stage a package receipt's developer tool without changing the installed executable."""
import json
import os
import plistlib
from pathlib import Path
import runpy
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
STAGE = runpy.run_path(str(REPO / "tools/hao-prepare-components"))["stage"]


class ComponentPreparation(unittest.TestCase):
    def test_install_replacement_and_refusal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            volume = root / "volume"
            directory = volume / "Library/Development Tools"
            directory.mkdir(parents=True)
            original = directory / "test-tool"
            entry = str(original.relative_to(volume))
            stub = Path(os.environ.get("B", REPO / "build")).resolve() / "hao-stub"
            info = {"pkgid": "local.fixture", "pkg-version": "1", "install-location": ""}
            entries = [entry]

            def receipt(_volume, _package):
                return dict(info), list(entries)

            def prepare(provider=receipt):
                return STAGE(volume, info["pkgid"], root / "output", stub, None, provider)

            def install(marker):
                subprocess.run(["clang", "-arch", "arm64", "-x", "c", "-", "-o", original],
                               input=f"int main(void) {{ return {marker}; }}", text=True, check=True)
                subprocess.run(["codesign", "--force", "--sign", "-", original], check=True, capture_output=True)

            install(1)
            # The immutable user flag can be exercised without changing protected system files.
            os.chflags(original, original.stat().st_flags | stat.UF_IMMUTABLE)
            try:
                refused = json.loads(prepare().read_text())
                self.assertIn("protected Darwin file flags", refused["refused"][entry])
            finally:
                os.chflags(original, original.stat().st_flags & ~stat.UF_IMMUTABLE)
            if os.geteuid() != 0:  # root may execute a file with any execute bit
                original.chmod(0o641)
                try:
                    refused = json.loads(prepare().read_text())
                    self.assertIn("cannot execute", refused["refused"][entry])
                finally:
                    original.chmod(0o755)
            source = original.read_bytes()
            first = prepare()
            manifest = json.loads(first.read_text())
            self.assertIn(entry, manifest["files"], manifest)
            self.assertEqual(manifest["files"][entry]["original_mode"], 0o755)
            self.assertFalse((first.parent / manifest["files"][entry]["object"]).stat().st_mode & 0o111)
            self.assertEqual(original.read_bytes(), source)
            self.assertEqual(prepare(), first)
            info["pkg-version"] = "2"
            install(2)
            second = prepare()
            self.assertNotEqual(second, first)
            calls = 0
            def changing_receipt(vol, pkg):
                nonlocal calls
                calls += 1
                if calls == 2:
                    original.chmod(0o4755)
                return receipt(vol, pkg)
            with self.assertRaisesRegex(RuntimeError, "changed during preparation"):
                prepare(changing_receipt)
            original.chmod(0o4755)
            refused = json.loads(prepare().read_text())
            self.assertFalse(refused["files"])
            self.assertIn("set-ID", refused["refused"][entry])
            original.chmod(0o755)
            subprocess.run(["/usr/bin/xattr", "-w", "com.apple.quarantine", "0081;00000000;Test;", original], check=True)
            refused = json.loads(prepare().read_text())
            self.assertIn("quarantine", refused["refused"][entry])
            subprocess.run(["/usr/bin/xattr", "-d", "com.apple.quarantine", original], check=True)
            with patch.object(subprocess, "check_output",
                              side_effect=subprocess.CalledProcessError(1, "xattr", output="failed access")):
                refused = json.loads(prepare().read_text())
                self.assertIn("failed access", refused["refused"][entry])
            output = root / "output"
            subprocess.run(["chmod", "+a", "everyone allow add_file", output], check=True)
            try:
                with self.assertRaisesRegex(RuntimeError, "ACL"):
                    prepare()
            finally:
                subprocess.run(["chmod", "-N", output], check=True)
            entitlements = root / "public-entitlements.plist"
            entitlements.write_bytes(plistlib.dumps({"com.apple.security.get-task-allow": True}))
            subprocess.run(["codesign", "--force", "--sign", "-", "--entitlements", entitlements, original],
                           check=True, capture_output=True)
            self.assertIn(entry, json.loads(prepare().read_text())["files"])
            # Only extract the signed ARM slice; the publisher-signed control is never run.
            publisher = directory / "publisher-control"
            subprocess.run(["lipo", "/usr/bin/true", "-thin", "arm64e", "-output", publisher], check=True)
            entries.append(str(publisher.relative_to(volume)))
            self.assertIn(entries[-1], json.loads(prepare().read_text())["files"])
            app = directory / "Fixture.app"
            main = app / "Contents/MacOS/Fixture"
            main.parent.mkdir(parents=True)
            main.write_bytes(original.read_bytes())
            main.chmod(0o755)
            info_file = app / "Contents/Info.plist"
            info_file.write_bytes(plistlib.dumps({"CFBundleExecutable": "Fixture", "CFBundlePackageType": "APPL",
                                                  "CFBundleIdentifier": "local.fixture.App"}))
            subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)
            entries.append(str(main.relative_to(volume)))
            bundle_manifest = prepare()
            self.assertEqual(prepare(), bundle_manifest)
            row = json.loads(bundle_manifest.read_text())["files"][entries[-1]]
            extraction = root / "delivered"
            extraction.mkdir()
            subprocess.run(["tar", "-xf", bundle_manifest.parent / row["context_object"], "-C", extraction],
                           check=True)
            for arch in ("arm64", "x86_64"):
                subprocess.run(["codesign", "--verify", "--strict", "-a", arch,
                                extraction / app.name / "Contents/MacOS/Fixture"], check=True)
            nested = app / "Contents/Resources/Nested"
            nested.parent.mkdir()
            nested.write_bytes(b"fixture")
            subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)
            subprocess.run(["/usr/bin/xattr", "-w", "com.apple.quarantine", "0081;00000000;Test;", nested],
                           check=True)
            rejected = json.loads(prepare().read_text())
            self.assertNotIn(entries[-1], rejected["files"])
            self.assertIn("quarantined member", rejected["refused"][entries[-1]])
            subprocess.run(["/usr/bin/xattr", "-d", "com.apple.quarantine", nested], check=True)
            subprocess.run(["chmod", "+a", "everyone deny execute", nested], check=True)
            try:
                denied = json.loads(prepare().read_text())
                self.assertNotIn(entries[-1], denied["files"])
                self.assertIn("ACL-bearing member", denied["refused"][entries[-1]])
            finally:
                subprocess.run(["chmod", "-N", nested], check=True)
            nested.chmod(0o4755)
            try:
                denied = json.loads(prepare().read_text())
                self.assertNotIn(entries[-1], denied["files"])
                self.assertIn("set-ID member", denied["refused"][entries[-1]])
            finally:
                nested.chmod(0o644)
            calls = 0
            def changing_sidecar(vol, pkg):
                nonlocal calls
                calls += 1
                if calls == 2:
                    info_file.write_bytes(info_file.read_bytes() + b"\n")
                return receipt(vol, pkg)
            with self.assertRaisesRegex(RuntimeError, "signed bundle resource changed"):
                prepare(changing_sidecar)
            entries.append("../outside")
            with self.assertRaisesRegex(ValueError, "invalid receipt path"):
                prepare()


if __name__ == "__main__":
    unittest.main()
