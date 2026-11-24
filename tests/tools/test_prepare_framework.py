"""macOS integration test for the file-backed framework packager."""
import hashlib
import plistlib
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hao_codesig
import hao_fat


@unittest.skipUnless(sys.platform == "darwin", "requires the Darwin signing and linker tools")
class FrameworkPreparation(unittest.TestCase):
    def test_prepare_and_refuse_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run([sys.executable, ROOT / "tests/guest/framework-xpc/build.py",
                            root / "apps"], check=True)
            path = Path("Probe.app/Contents/Frameworks/ProbeKit.framework")
            source = root / "apps/translated" / path
            native = root / "apps/native" / path
            output = root / source.name
            tool = ROOT / "tools/hao-prepare-framework"

            def prepare(src):
                return subprocess.run([tool, src, output], capture_output=True)

            self.assertNotEqual(prepare(native).returncode, 0)
            self.assertFalse(output.exists())
            arm = (source / "ProbeKit").read_bytes()
            result = prepare(source)
            self.assertEqual(result.returncode, 0, result.stderr)
            slices = {cpu: blob for cpu, _, blob in hao_fat.slices((output / "ProbeKit").read_bytes())}
            self.assertEqual(slices[hao_fat.CPU_ARM64], arm)
            host = slices[hao_fat.CPU_X86_64]
            sig = hao_codesig.check(arm)
            marker = b"haoslice" + bytes((1, sig["hash_type"], 0, 0)) + sig["cdhash"]
            self.assertEqual(host.count(marker), 1)
            self.assertEqual(hao_codesig.check(host)["verdict"], "valid")
            host_file = root / "host"
            host_file.write_bytes(host)
            exports = subprocess.check_output(["/usr/bin/nm", "-gU", host_file])
            self.assertEqual(exports.strip(), b"")
            for resource in source.rglob("*"):
                if resource.is_symlink() or not resource.is_file() or resource.read_bytes() == arm:
                    continue
                self.assertEqual(resource.read_bytes(), (output / resource.relative_to(source)).read_bytes())
            original = hashlib.sha256((output / "ProbeKit").read_bytes()).digest()
            self.assertNotEqual(prepare(source).returncode, 0)
            self.assertEqual(hashlib.sha256((output / "ProbeKit").read_bytes()).digest(), original)
            quarantined_parent = source.parent
            subprocess.run(["xattr", "-w", "com.apple.quarantine", "0081;00000000;fixture;",
                            quarantined_parent], check=True)
            broken_dest = root / "other" / source.name
            broken_dest.parent.mkdir()
            self.assertNotEqual(subprocess.run([tool, source, broken_dest], capture_output=True).returncode, 0)
            self.assertFalse(broken_dest.exists())
            subprocess.run(["xattr", "-d", "com.apple.quarantine", quarantined_parent], check=True)
            engine = runpy.run_path(str(tool))["prepare"]
            verify = engine.__globals__["verify"]
            def quarantine_after_copy(path):
                verify(path)
                subprocess.run(["xattr", "-w", "com.apple.quarantine", "0081;00000000;fixture;",
                                quarantined_parent], check=True)
            engine.__globals__["verify"] = quarantine_after_copy
            with self.assertRaisesRegex(ValueError, "quarantine"):
                engine(source, broken_dest, "-")
            self.assertFalse(broken_dest.exists())
            subprocess.run(["xattr", "-d", "com.apple.quarantine", quarantined_parent], check=True)
            nested = source / "Versions/A/Resources/Info.plist"
            def quarantine_member(path):
                verify(path)
                subprocess.run(["xattr", "-w", "com.apple.quarantine", "0081;00000000;fixture;",
                                nested], check=True)
            engine.__globals__["verify"] = quarantine_member
            with self.assertRaisesRegex(ValueError, "quarantin"):
                engine(source, broken_dest, "-")
            subprocess.run(["xattr", "-d", "com.apple.quarantine", nested], check=True)
            self.assertFalse(broken_dest.exists())
            service = next((source / "XPCServices").glob("*.xpc"))
            declared = root / "HaoOwnedDiscovery.framework"
            refused = root / "OtherOwner.framework"
            result = subprocess.run([tool, "--separate-adapter", "--advertise-service", service.name,
                                     source, refused], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(refused.exists())
            for invalid in ("../" + service.name, "missing.xpc"):
                result = subprocess.run([tool, "--separate-adapter", "--advertise-service", invalid,
                                         source, declared], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(declared.exists())
            subprocess.run([tool, "--separate-adapter", "--advertise-service", service.name,
                            source, declared], check=True)
            service_files = [p.relative_to(service) for p in service.rglob("*") if p.is_file()]
            for location in (declared / "XPCServices" / service.name,
                             declared / "Frameworks/ProbeKit.framework/XPCServices" / service.name):
                for relative in service_files:
                    self.assertEqual((location / relative).read_bytes(), (service / relative).read_bytes())
                subprocess.run(["codesign", "--verify", "--strict", "--all-architectures", location],
                               check=True)
            plain_source = root / "plain" / source.name
            plain_source.parent.mkdir()
            shutil.copytree(source, plain_source, symlinks=True)
            plain_service = plain_source / "XPCServices" / service.name
            plain_info = plain_service / "Contents/Info.plist"
            metadata = plistlib.loads(plain_info.read_bytes())
            del metadata["HaoAdvertisedAdapterIdentifiers"]
            plain_info.write_bytes(plistlib.dumps(metadata))
            subprocess.run(["codesign", "-fs", "-", plain_service], check=True)
            subprocess.run(["codesign", "-fs", "-", plain_source], check=True)
            acl_dir = (plain_source / "XPCServices").resolve()
            subprocess.run(["chmod", "+a", "user:nobody deny search", acl_dir], check=True)
            result = subprocess.run([tool, "--separate-adapter", "--advertise-service", service.name,
                                     plain_source, refused], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(refused.exists())
            subprocess.run(["chmod", "-a", "user:nobody deny search", acl_dir], check=True)
            gate = root / "Gate"
            gate.mkdir()
            alias_source = root / "Alias.framework"
            alias_source.symlink_to(Path("Gate/..") / plain_source.relative_to(root))
            subprocess.run(["chmod", "+a", "user:nobody deny search", gate], check=True)
            result = subprocess.run([tool, "--separate-adapter", "--advertise-service", service.name,
                                     alias_source, refused], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"enclosing access restriction", result.stderr)
            self.assertFalse(refused.exists())
            subprocess.run(["chmod", "-a", "user:nobody deny search", gate], check=True)
            subprocess.run([tool, "--separate-adapter", "--advertise-service", service.name,
                            plain_source, refused], check=True)
            adapter = root / "OwnedAdapter.framework"
            wrong = subprocess.run([tool, "--separate-adapter", "--associate-original",
                                    native / "ProbeKit", source, adapter], capture_output=True)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertFalse(adapter.exists())
            result = subprocess.run([tool, "--separate-adapter", "--associate-original",
                                     source / "ProbeKit", source, adapter], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((adapter / "Frameworks/ProbeKit.framework/ProbeKit").read_bytes(), arm)
            self.assertEqual([cpu for cpu, _, _ in hao_fat.slices((adapter / "OwnedAdapter").read_bytes())],
                             [hao_fat.CPU_X86_64])
            assoc = plistlib.loads((adapter / "Resources/Info.plist").read_bytes())["HaoOriginalAssociation"]
            self.assertEqual(assoc, dict(Version=1, Path=str((source / "ProbeKit").resolve()),
                InstallName="@rpath/ProbeKit.framework/Versions/A/ProbeKit",
                BundleIdentifier="org.hao.fixture.frameworkxpc.bulk.translated.kit",
                SigningIdentifier=sig["ident"].decode(), HashType=sig["hash_type"],
                CDHash=sig["cdhash"], SHA256=hashlib.sha256(arm).digest(),
                AdapterIdentifier="org.hao.adapter.ownedadapter"))
            record_tool = ROOT / "tools/hao-companion-record"
            output_record = root / "launcher-record"
            subprocess.run([record_tool, "-o", output_record, adapter], check=True)
            host_sig = hao_codesig.check((adapter / "OwnedAdapter").read_bytes())
            strings = (assoc["Path"], assoc["InstallName"], str((adapter / "OwnedAdapter").resolve()))
            expected_record = (b"haocomp1" + bytes([sig["hash_type"]]) + sig["cdhash"]
                               + bytes([host_sig["hash_type"]]) + host_sig["cdhash"]
                               + assoc["SHA256"] + b"".join(x.encode() + b"\0" for x in strings))
            self.assertEqual(output_record.read_bytes(), expected_record)
            duplicate = root / "duplicates"
            result = subprocess.run([record_tool, "-o", duplicate, adapter, adapter], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(duplicate.exists())
            # Link-only libraries exercise launcher packaging without running a guest or service.
            mock = root / "mock.c"
            mock.write_text("int hao_cli_main(int n, char **v, char **e) { return 0; }\n")
            for lib in ("libhao", "libhao_identity"):
                subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-dynamiclib", str(mock),
                                "-install_name", "@rpath/" + lib + ".dylib", "-o",
                                root / (lib + ".dylib")], check=True)
            launcher = root / "OwnedLauncher"
            subprocess.run([ROOT / "tools/hao-companion-launcher", root / "apps/translated/Probe.app/Contents/MacOS/Probe",
                            adapter, "--runtime", root, "-o", launcher], check=True)
            deps = subprocess.check_output(["otool", "-L", launcher], text=True)
            self.assertIn(str((adapter / "OwnedAdapter").resolve()), deps)
            self.assertEqual(launcher.read_bytes().count(expected_record), 1)
            guest_main = root / "apps/translated/Probe.app/Contents/MacOS/Probe"
            sig_main = hao_codesig.check(guest_main.read_bytes())
            main_marker = b"haoslice" + bytes((1, sig_main["hash_type"], 0, 0)) + sig_main["cdhash"]
            self.assertEqual(launcher.read_bytes().count(main_marker), 1)
            alias = root / "original-link"
            alias.symlink_to(source / "ProbeKit")
            identical = root / "identical"
            identical.write_bytes(arm)
            raced = root / "RacedAdapter.framework"
            adapter_engine = runpy.run_path(str(tool))["prepare_adapter"]
            def retarget_after_sign(path):
                verify(path)
                if path.name == raced.name:
                    alias.unlink()
                    alias.symlink_to(identical)
            adapter_engine.__globals__["verify"] = retarget_after_sign
            with self.assertRaisesRegex(ValueError, "path changed"):
                adapter_engine(source, raced, "-", alias)
            self.assertFalse(raced.exists())
            metadata = plistlib.loads(nested.read_bytes())
            metadata["CFBundleIdentifier"] = "org.hao.adapter.collidingadapter"
            nested.write_bytes(plistlib.dumps(metadata))
            subprocess.run(["codesign", "--force", "--sign", "-", source], check=True)
            collision = root / "CollidingAdapter.framework"
            result = subprocess.run([tool, "--separate-adapter", source, collision], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"identifier collides", result.stderr)
            self.assertFalse(collision.exists())
            nested.write_bytes(b"broken")
            self.assertNotEqual(subprocess.run([tool, source, broken_dest], capture_output=True).returncode, 0)
            self.assertFalse(broken_dest.exists())


if __name__ == "__main__":
    unittest.main()
