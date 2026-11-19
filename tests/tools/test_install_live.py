"""hao-runtime-on-mac --install-live: an installed system (hao-install.py on a first payload) updated in place to a
second payload; only what differs is written; payloads for another system, dirty payloads and systems without
install.json are refused before anything is written.

  python3 -I tests/tools/test_install_live.py        (tests/tools/run.sh runs it)

SYSTEM and PAYLOAD are synthetic (no Apple binaries, no compiler). The running-Mac path (csrutil, mount_apfs,
bless) is covered only as far as the commands --dry-run prints: nothing here mounts, blesses or runs csrutil.
"""
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
import hao_fat  # noqa: E402

INSTALL = TOOLS / 'hao-install.py'
ONMAC = TOOLS / 'hao-runtime-on-mac'
BUILD, CACHE = '27A101', 'AB' * 16


def executable(cpu, uuid, body=b''):
    return struct.pack('<IIIIIIII', 0xfeedfacf, cpu, 0, 2, 1, 24, 0, 0) + struct.pack('<II', 0x1b, 24) + bytes.fromhex(uuid) + body


def digest(data):
    return hashlib.sha256(data).hexdigest()


def hao_stub(uuid):
    """A thin x86_64 executable that links libhao.dylib without a __TEXT,__hao_slice record: a hao stub of an
    earlier build (tests/tools/test_install.py hao_stub)."""
    name = b'/usr/lib/hao/libhao.dylib\0\0\0\0\0\0\0'
    cmds = struct.pack('<II', 0x1b, 24) + bytes.fromhex(uuid) + struct.pack('<IIIIII', 0xc, 24 + len(name), 24, 0, 0, 0) + name
    return struct.pack('<IIIIIIII', 0xfeedfacf, hao_fat.CPU_X86_64, 3, 2, 2, len(cmds), 0, 0) + cmds + b'stub'


def aot_cache(guest, gen):
    """A v8 AOT cache header for GUEST (hex) made by the libhao whose LC_UUID is GEN (hex), bound to a cdhash."""
    h = bytearray(144)
    h[:8] = b'HAOAOT2\0'
    struct.pack_into('<II', h, 8, 8, 144)
    h[16:32], h[32:48] = bytes.fromhex(guest), bytes.fromhex(gen)
    struct.pack_into('<I', h, 96, 1)
    return bytes(h)


def make_payload(d, libhao_uuid, stub, guest, aot=(), **meta):
    """A payload at D as tools/hao-image-payload lays it out, with payload-manifest.json's identity fields."""
    shutil.rmtree(d, ignore_errors=True)
    for sub in ('root/usr/lib/hao/aot', 'root/usr/libexec', 'embed/stubs'):
        (d / sub).mkdir(parents=True)
    (d / 'root/usr/lib/hao/libhao.dylib').write_bytes(executable(hao_fat.CPU_X86_64, libhao_uuid, b'libhao'))
    (d / 'root/usr/libexec/hao').write_bytes(b'launcher')
    (d / 'root/usr/libexec/hao').chmod(0o755)
    (d / 'root/usr/lib/hao/trusted-cdhashes').write_bytes(b'# none\n')
    for g in aot:
        (d / 'root/usr/lib/hao/aot' / (g + '.aot')).write_bytes(aot_cache(g, libhao_uuid))
    (d / 'embed/stubs/prog.stub').write_bytes(stub)
    entry = dict(path='usr/bin/prog', stub='stubs/prog.stub', stub_sha256=digest(stub), source_sha256=digest(guest),
                 result_sha256=digest(hao_fat.combine(guest, stub)))
    (d / 'embed/manifest.json').write_text(json.dumps(dict(files=[entry], required=['usr/bin/prog'],
                                                           stub_sha256=digest(stub))))
    files = {}
    for base, _, names in os.walk(d):
        for n in names:
            f = os.path.join(base, n)
            files[os.path.relpath(f, d)] = digest(Path(f).read_bytes())
    m = dict(version='%s-%s' % (BUILD, meta.get('commit', 'c1')[:12]), commit='c1', clean=True, os_build=BUILD,
             cache_uuid=CACHE, layout_version=1, files=files)
    m.update(meta)
    (d / 'payload-manifest.json').write_text(json.dumps(m, indent=1, sort_keys=True))


def tree(root):
    """{path: (inode, mtime_ns, sha256)} of every file under ROOT."""
    out = {}
    for base, _, names in os.walk(root):
        for n in names:
            f = os.path.join(base, n)
            st = os.lstat(f)
            out[os.path.relpath(f, root)] = (st.st_ino, st.st_mtime_ns, digest(Path(f).read_bytes()))
    return out


class InstallLive(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.system, self.p1, self.p2 = self.base / 'system', self.base / 'p1', self.base / 'p2'
        self.guest = executable(hao_fat.CPU_ARM64, '2' * 32, b'guest')
        self.stub = executable(hao_fat.CPU_X86_64, '5' * 32, b'stub')
        for d in ('usr/bin', 'usr/lib/hao', 'System/Library/CoreServices', 'System/Library/dyld'):
            (self.system / d).mkdir(parents=True)
        (self.system / 'usr/bin/prog').write_bytes(self.guest)
        (self.system / 'usr/bin/other').write_bytes(b'not hao\'s')
        (self.system / 'System/Library/CoreServices/SystemVersion.plist').write_bytes(
            plistlib.dumps(dict(ProductBuildVersion=BUILD, ProductVersion='27.0')))
        cache = bytearray(0x200)
        cache[:16] = b'dyld_v1  x86_64\0'
        cache[0x58:0x68] = bytes.fromhex(CACHE)
        (self.system / 'System/Library/dyld/dyld_shared_cache_x86_64').write_bytes(bytes(cache))
        self.aot = 'a' * 32
        self.pristine = self.base / 'pristine'
        shutil.copytree(self.system, self.pristine)
        make_payload(self.p1, '1' * 32, self.stub, self.guest, aot=[self.aot])
        p = subprocess.run([sys.executable, '-I', str(INSTALL), str(self.system), str(self.p1)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout)
        self.assertTrue((self.system / 'usr/lib/hao/aot' / (self.aot + '.aot')).is_file())
        self.before = tree(self.system)

    def live(self, *args):
        p = subprocess.run([sys.executable, '-I', str(ONMAC), '--install-live', *map(str, args)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertNotIn('Traceback', p.stdout)
        return p.returncode, p.stdout

    def changed(self):
        after = tree(self.system)
        return sorted(k for k in set(self.before) | set(after) if self.before.get(k) != after.get(k))

    def test_libhao_only(self):
        """a payload that differs in libhao only: libhao.dylib and install.json are written, the AOT cache of the
        old libhao is removed, nothing else is touched (the embedded program keeps its inode and bytes)"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.changed(), ['usr/lib/hao/aot/%s.aot' % self.aot, 'usr/lib/hao/install.json',
                                          'usr/lib/hao/libhao.dylib'], out)
        self.assertFalse((self.system / 'usr/lib/hao/aot' / (self.aot + '.aot')).exists())
        self.assertEqual((self.system / 'usr/lib/hao/libhao.dylib').read_bytes(),
                         (self.p2 / 'root/usr/lib/hao/libhao.dylib').read_bytes())
        self.assertIn('1 files written, 0 programs embedded, 0 stubs restubbed, 0 files replaced, 1 AOT caches removed', out)
        rec = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        self.assertEqual((rec['payload']['commit'], rec['payload']['os_build'], rec['live']['root_written'],
                          rec['aot_caches']['removed_stale'], rec['aot_caches']['libhao_uuid'], rec['errors']),
                         ('c2', BUILD, ['root/usr/lib/hao/libhao.dylib'], [self.aot + '.aot'], '3' * 32, []))
        self.assertEqual(rec['embedded'], 1)
        self.before = tree(self.system)
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 0, out)
        self.assertIn('installed already; nothing to do', out)
        self.assertEqual(self.changed(), [])

    def fresh_record(self, payload):
        """install.json as hao-install.py writes it for PAYLOAD on a copy of the system before any install"""
        d = self.base / 'fresh'
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(self.pristine, d)
        p = subprocess.run([sys.executable, '-I', str(INSTALL), str(d), str(payload)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout)
        return json.loads((d / 'usr/lib/hao/install.json').read_text())

    def assert_record_as_fresh(self, payload):
        """the live record is the one a fresh hao-install.py of PAYLOAD writes, plus "live" (and the old caches it
        removed: a fresh install has none to remove)"""
        rec = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        self.assertIn('live', rec)
        def same(r):
            return {k: dict(v, removed_stale=None) if k == 'aot_caches' else v for k, v in r.items() if k != 'live'}
        self.assertEqual(same(rec), same(self.fresh_record(payload)))
        return rec

    def test_record_as_fresh_install(self):
        """after a libhao-only update install.json holds what hao-install.py writes for the new payload (every
        runtime file counted, the identity of the payload), not only what this run wrote"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 0, out)
        rec = self.assert_record_as_fresh(self.p2)
        self.assertEqual((rec['installed_files'], rec['payload']['commit'], rec['live']['previous']['commit']),
                         (3, 'c2', 'c1'))

    def test_libhao_swapped_by_hand(self):
        """the state of a Mac whose libhao was replaced by hand (a live swap made permanent) and whose install.json
        was edited to name it: the new payload's libhao is there already, the AOT caches on disk are the old
        libhao's, install.json has no payload identity. The update writes the payload's AOT caches (made by the
        new libhao, same names) and install.json, and nothing else; the record is a fresh install's."""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, aot=[self.aot], commit='c2')
        lib = self.system / 'usr/lib/hao/libhao.dylib'
        lib.write_bytes((self.p2 / 'root/usr/lib/hao/libhao.dylib').read_bytes())
        rec = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        del rec['payload']
        rec['aot_caches'] = dict(libhao_uuid='3' * 32, caches=0, bytes=0, removed_stale=[])
        (self.system / 'usr/lib/hao/install.json').write_text(json.dumps(rec, indent=1) + '\n')
        self.before = tree(self.system)
        rc, out = self.live(self.p2, '--system', self.system, '--dry-run')
        self.assertEqual(rc, 0, out)
        self.assertIn('would write usr/lib/hao/aot/%s.aot' % self.aot, out)
        self.assertNotIn('would remove AOT cache', out)
        self.assertNotIn('would write usr/lib/hao/libhao.dylib', out)
        self.assertEqual(self.changed(), [])
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.changed(), ['usr/lib/hao/aot/%s.aot' % self.aot, 'usr/lib/hao/install.json'], out)
        self.assertIn('1 files written, 0 programs embedded, 0 stubs restubbed, 0 files replaced, 0 AOT caches removed', out)
        rec = self.assert_record_as_fresh(self.p2)
        self.assertEqual(rec['aot_caches'], dict(libhao_uuid='3' * 32, caches=1, bytes=144, removed_stale=[]))
        self.assertIsNone(rec['live']['previous'])
        self.before = tree(self.system)
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertIn('installed already; nothing to do', out)
        self.assertEqual(self.changed(), [])

    def test_restub(self):
        """a program carrying a hao stub of another build gets this payload's stub in place (hao-install.py's
        restub rule); the program keeps its inode"""
        legacy = hao_fat.combine(self.guest, hao_stub('6' * 32))
        prog = self.system / 'usr/bin/prog'
        with open(prog, 'r+b') as f:
            f.write(legacy); f.truncate()
        self.before = tree(self.system)
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.changed(), ['usr/bin/prog', 'usr/lib/hao/aot/%s.aot' % self.aot,
                                          'usr/lib/hao/install.json', 'usr/lib/hao/libhao.dylib'], out)
        self.assertEqual(prog.read_bytes(), hao_fat.combine(self.guest, self.stub))
        self.assertEqual(prog.stat().st_ino, self.before['usr/bin/prog'][0])
        self.assertIn('1 stubs restubbed', out)
        self.assertEqual(json.loads((self.system / 'usr/lib/hao/install.json').read_text())['restubbed'], ['usr/bin/prog'])

    def test_dry_run(self):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--system', self.system, '--dry-run')
        self.assertEqual(rc, 0, out)
        for line in ('would write usr/lib/hao/libhao.dylib', 'would remove AOT cache %s.aot' % self.aot,
                     'would write usr/lib/hao/install.json'):
            self.assertIn('hao-runtime-on-mac: ' + line, out)
        self.assertNotIn('would write usr/libexec/hao', out)
        self.assertEqual(self.changed(), [])

    def test_running_mac_dry_run(self):
        """without --system: the commands of the running-Mac path are printed, none is run, nothing is written
        (--booted stands in for /)"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--booted', self.system, '--dry-run')
        self.assertEqual(rc, 0, out)
        lines = [l for l in out.splitlines() if l.startswith('+ ')]
        self.assertEqual(lines[0], '+ /usr/bin/csrutil authenticated-root status')
        self.assertRegex(lines[1], r'^\+ /sbin/mount_apfs -o nobrowse \S+ /private/var/run/hao-live\.XXXXXX$')
        self.assertEqual(lines[2:], ['+ /usr/sbin/bless --mount /private/var/run/hao-live.XXXXXX --bootefi --create-snapshot',
                                     '+ /sbin/umount /private/var/run/hao-live.XXXXXX'])
        self.assertIn('would write usr/lib/hao/libhao.dylib', out)
        self.assertEqual(self.changed(), [])

    @unittest.skipIf(os.geteuid() == 0, 'as root the real path would run')
    def test_running_mac_needs_root(self):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out = self.live(self.p2, '--booted', self.system)
        self.assertEqual(rc, 1, out)
        self.assertIn('install-live refused: run it as root', out)
        self.assertNotIn('+ ', out)
        self.assertEqual(self.changed(), [])

    def refused(self, needle, **meta):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, **meta)
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 1, out)
        self.assertIn('hao-runtime-on-mac: install-live refused: ', out)
        self.assertIn(needle, out)
        self.assertEqual(self.changed(), [])

    def test_other_os_build(self):
        self.refused('the payload is for os_build 27A999, the installed system has %s' % BUILD, os_build='27A999')

    def test_other_cache_uuid(self):
        self.refused('the payload is for x86_64 cache UUID %s' % ('CD' * 16), cache_uuid='CD' * 16)

    def test_dirty_payload(self):
        self.refused('the payload is dirty', clean=False)

    def test_no_install_json(self):
        (self.system / 'usr/lib/hao/install.json').unlink()
        self.before = tree(self.system)
        self.refused('install.json missing: hao was not installed on this system by hao-install.py')

    def test_changed_payload_file(self):
        """a payload file that is not what its manifest lists: hao-install.py's inventory refuses it"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        (self.p2 / 'root/usr/libexec/hao').write_bytes(b'changed')
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 1, out)
        self.assertIn('payload root/usr/libexec/hao does not match its sha256', out)
        self.assertEqual(self.changed(), [])

    def test_embed_error_before_first_write(self):
        """a program the payload cannot embed (another source) refuses the update before any file is written"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        (self.system / 'usr/bin/prog').write_bytes(executable(hao_fat.CPU_ARM64, '9' * 32, b'other'))
        self.before = tree(self.system)
        rc, out = self.live(self.p2, '--system', self.system)
        self.assertEqual(rc, 1, out)
        self.assertIn('errors before the first write; nothing was written', out)
        self.assertEqual(self.changed(), [])

    def test_authenticated_root_status(self):
        loader = importlib.machinery.SourceFileLoader('onmac', str(ONMAC))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader('onmac', loader))
        loader.exec_module(m)
        self.assertIs(m.authenticated_root_on('Authenticated Root status: enabled\n'), True)
        self.assertIs(m.authenticated_root_on('Authenticated Root status: disabled\n'), False)
        self.assertIsNone(m.authenticated_root_on('csrutil: failed to read\n'))


if __name__ == '__main__':
    unittest.main()
