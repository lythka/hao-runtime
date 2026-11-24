"""Exercise installed-app discovery with signed fixtures, without launching them."""
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[2] / "tools/hao-app-inventory"


class AppInventory(unittest.TestCase):
    def test_replacement_and_signature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "Marker.app"
            executable = app / "Contents/MacOS/Marker"
            executable.parent.mkdir(parents=True)
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
                "CFBundleExecutable": "Marker", "CFBundleIdentifier": "test.hao.Marker",
                "CFBundlePackageType": "APPL",
            }))
            source = root / "marker.c"

            def install(marker, architectures):
                source.write_text(f'char marker[] = "{marker}"; int main(void) {{ return marker[0]; }}')
                flags = [arg for arch in architectures for arg in ("-arch", arch)]
                subprocess.run(["clang", *flags, source, "-o", executable], check=True)
                subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)

            def discover():
                return json.loads(subprocess.check_output([TOOL, root]))

            install("inventory-one", ["arm64"])
            invalid = root / "Malformed.app/Contents"
            (invalid / "MacOS").mkdir(parents=True)
            (invalid / "Info.plist").write_text("<plist><date>invalid</date></plist>")
            first = discover()
            self.assertEqual(first, discover())
            self.assertEqual(first[0]["executable_sha256"], hashlib.sha256(executable.read_bytes()).hexdigest())
            self.assertEqual(first[0]["architectures"], ["arm64"])
            executable.rename(root / "pending")
            self.assertEqual(discover(), [])
            (root / "pending").rename(executable)
            self.assertEqual(discover(), first)
            install("inventory-two", ["arm64", "x86_64"])
            second = discover()
            self.assertNotEqual(first[0]["executable_sha256"], second[0]["executable_sha256"])
            self.assertEqual(set(second[0]["architectures"]), {"arm64", "x86_64"})
            executable.write_bytes(executable.read_bytes().replace(b"inventory-two", b"inventory-bad"))
            self.assertEqual(discover(), [])


if __name__ == "__main__":
    unittest.main()
