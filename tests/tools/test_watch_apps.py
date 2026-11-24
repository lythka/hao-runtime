"""Exercise automatic preparation after installation, update, and removal."""
import os
from pathlib import Path
import plistlib
import runpy
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
reconcile = runpy.run_path(str(REPO / "tools/hao-watch-apps"))["reconcile"]


class AppUpdates(unittest.TestCase):
    def test_install_update_and_quarantine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            install = root / "Applications"
            output = root / "Prepared"
            app = install / "Fixture.app"
            main = app / "Contents/MacOS/Fixture"
            main.parent.mkdir(parents=True)
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": "org.hao.fixture", "CFBundleExecutable": "Fixture",
                "CFBundlePackageType": "APPL", "CFBundleVersion": "1",
            }))
            resources = app / "Contents/Resources"
            resources.mkdir()
            (resources / "value").write_text("content")
            alias = resources / "internal"
            alias.symlink_to("value")
            os.chmod(alias, 0o777, follow_symlinks=False)
            stub = Path(os.environ.get("B", REPO / "build")).resolve() / "hao-stub"
            engine = runpy.run_path(str(REPO / "tools/hao-embed"))
            local_cache = next(Path(path) for path in engine["HOST_X86_CACHES"]
                               if (Path(path) / engine["X86_CACHE_FILE"]).is_file())
            cache = Path(os.path.relpath(local_cache))
            link = output / app.name

            def publish(marker):
                subprocess.run(["clang", "-arch", "arm64", "-x", "c", "-", "-o", main],
                               input=f"int main(void) {{return {marker};}}", text=True, check=True)
                subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)
                original = main.read_bytes()
                reconcile(install, output, stub, cache)
                self.assertEqual(original, main.read_bytes())
                self.assertTrue(link.exists())

            alias = root / "alias"
            alias.symlink_to(install)
            with self.assertRaises(ValueError):
                reconcile(install, alias / "Nested", stub, cache)
            self.assertFalse((install / "Nested").exists())
            malformed = install / "Bad.app/Contents"
            malformed.mkdir(parents=True)
            (malformed / "Info.plist").write_bytes(b'<?xml version="1.0"?><plist><dict>')
            publish(1)
            self.assertEqual(os.readlink(link / "Contents/Resources/internal"), "value")
            first = link.resolve()
            reconcile(install, output, stub, cache)
            self.assertEqual(first, link.resolve())
            store = output / ".generations"
            subprocess.run(["chmod", "+a", "everyone allow add_file", store], check=True)
            try:
                reconcile(install, output, stub, cache)
                self.assertFalse(link.is_symlink())
            finally:
                subprocess.run(["chmod", "-N", store], check=True)
            reconcile(install, output, stub, cache)
            self.assertEqual(first, link.resolve())
            old_main = first / "Contents/MacOS/Fixture"
            old_main.write_bytes(b"corrupt")
            reconcile(install, output, stub, cache)
            self.assertFalse(link.is_symlink())
            publish(2)
            self.assertNotEqual(first, link.resolve())
            subprocess.run(["xattr", "-w", "com.apple.quarantine", "0081;00000000;fixture;", app], check=True)
            reconcile(install, output, stub, cache)
            self.assertFalse(link.is_symlink())
            self.assertTrue(app.exists())


if __name__ == "__main__":
    unittest.main()
