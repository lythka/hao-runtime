"""hao-install.py path rules: manifest names stay under SYSTEM and PAYLOAD, and
nothing is written or read through a symbolic link.

  python3 -I tests/tools/test_install.py        (tests/tools/run.sh runs it)

Each case builds a small synthetic SYSTEM and PAYLOAD (no Apple binaries, no
compiler), runs the installer as a subprocess and checks the exit status, the
message and that files outside SYSTEM kept their bytes.
"""
import hashlib
import importlib.machinery
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
import hao_fat  # noqa: E402

INSTALL = TOOLS / 'hao-install.py'


def executable(cpu, uuid, body=b''):
    return struct.pack('<IIIIIIII', 0xfeedfacf, cpu, 0, 2, 1, 24, 0, 0) + struct.pack('<II', 0x1b, 24) + bytes.fromhex(uuid) + body


def digest(data):
    return hashlib.sha256(data).hexdigest()


class InstallPaths(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.system, self.payload, self.outside = self.base / 'system', self.base / 'payload', self.base / 'outside'
        for d in (self.system / 'usr/bin', self.system / 'usr/lib/hao', self.payload / 'root/usr/lib/hao',
                  self.payload / 'root/usr/libexec', self.payload / 'embed/stubs', self.outside):
            d.mkdir(parents=True)
        self.guest = executable(hao_fat.CPU_ARM64, '2' * 32, b'guest')
        self.stub = executable(hao_fat.CPU_X86_64, '5' * 32, b'stub')
        self.result = hao_fat.combine(self.guest, self.stub)
        (self.system / 'usr/bin/prog').write_bytes(self.guest)
        (self.payload / 'root/usr/lib/hao/libhao.dylib').write_bytes(executable(hao_fat.CPU_X86_64, '1' * 32, b'libhao'))
        (self.payload / 'root/usr/libexec/hao').write_bytes(b'launcher')
        (self.payload / 'embed/stubs/prog.stub').write_bytes(self.stub)
        self.entry = dict(path='usr/bin/prog', stub='stubs/prog.stub', stub_sha256=digest(self.stub),
                          source_sha256=digest(self.guest), result_sha256=digest(self.result))
        self.manifest = dict(files=[self.entry], required=['usr/bin/prog'], stub_sha256=digest(self.stub))
        self.victim = self.outside / 'victim'
        self.victim.write_bytes(b'not the installer\'s to write')

    def run_install(self, replace=None):
        (self.payload / 'embed/manifest.json').write_text(json.dumps(self.manifest))
        if replace is not None:
            (self.payload / 'replace').mkdir(exist_ok=True)
            (self.payload / 'replace/manifest.json').write_text(json.dumps(replace))
        p = subprocess.run([sys.executable, '-I', str(INSTALL), str(self.system), str(self.payload)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        return p.returncode, p.stdout

    def refused(self, needle, replace=None):
        """the install fails with NEEDLE in a hao: error line, not a traceback, and the outside file is intact"""
        rc, out = self.run_install(replace)
        self.assertEqual(rc, 1, out)
        self.assertNotIn('Traceback', out)
        self.assertTrue(any(line.startswith('hao: error:') and needle in line for line in out.splitlines()), out)
        self.assertEqual(self.victim.read_bytes(), b'not the installer\'s to write')
        return out

    def untouched(self):
        """a refusal before the first write: no runtime file, no record, the program as it was"""
        self.assertFalse((self.system / 'usr/lib/hao/libhao.dylib').exists())
        self.assertFalse((self.system / 'usr/lib/hao/install.json').exists())
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.guest)

    def test_plain_install(self):
        rc, out = self.run_install()
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.result)
        record = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        self.assertEqual((record['installed_files'], record['embedded'], record['errors']), (2, 1, []))
        rc, out = self.run_install()
        self.assertEqual(rc, 0, out)

    def test_aot_cpu_profiles(self):
        """Only base/AES generator identities survive installation."""
        version = next(int(line.split()[-1]) for line in
                       (TOOLS.parent / 'include/hao/aot.h').read_text().splitlines()
                       if line.startswith('#define HAO_AOT_VERSION '))
        cache = self.payload / 'root/usr/lib/hao/aot'
        cache.mkdir()
        names = []
        for i, (byte, flip) in enumerate(((15, 0), (15, 1), (15, 2), (15, 128), (0, 1))):
            guest = bytes.fromhex(f'{i + 10:032x}')
            h = bytearray(136); h[:8] = b'HAOAOT2\0'
            struct.pack_into('<II', h, 8, version, 136)
            h[16:32] = guest; h[32:48] = bytes.fromhex('1' * 32)
            h[32 + byte] ^= flip
            struct.pack_into('<I', h, 96, 1)
            name = guest.hex() + '.aot'; names.append(name)
            (cache / name).write_bytes(h)
        rc, out = self.run_install()
        self.assertEqual(rc, 0, out)
        installed = self.system / 'usr/lib/hao/aot'
        self.assertEqual(sorted(p.name for p in installed.iterdir()), names[:2])
        record = json.loads((self.system / 'usr/lib/hao/install.json').read_text())['aot_caches']
        self.assertEqual(record['caches'], 2)
        self.assertEqual(record['removed_stale'], names[2:])

    def test_manifest_names_are_relative(self):
        other = self.outside / 'prog'
        other.write_bytes(self.guest)
        for key, name in (('path', str(other)), ('path', '../outside/prog'), ('path', 'usr/bin/../../../outside/prog'),
                          ('path', 'usr//bin/prog'), ('path', './usr/bin/prog'), ('path', 'usr\\bin\\prog'), ('path', ''),
                          ('path', 7), ('stub', str(self.payload / 'embed/stubs/prog.stub')), ('stub', '../embed/stubs/prog.stub'),
                          ('stub', 'stubs/../../../outside/victim')):
            with self.subTest(key=key, name=name):
                self.manifest['files'] = [dict(self.entry, **{key: name})]
                self.refused('install refused: embed/manifest.json')
                self.untouched()
                self.assertEqual(other.read_bytes(), self.guest)

    def test_bound_file_name_is_relative(self):
        for name in (str(self.victim), '../outside/victim'):
            with self.subTest(name=name):
                self.manifest['files'] = [dict(self.entry, bound=[dict(slot=1, file=name, sha256='0' * 64)])]
                self.refused('install refused: embed/manifest.json')
                self.untouched()

    def test_replace_names_are_relative(self):
        new = b'replacement'
        (self.payload / 'replace').mkdir()
        (self.payload / 'replace/new').write_bytes(new)
        (self.outside / 'new').write_bytes(new)
        good = dict(path='usr/bin/prog', file='new', source_sha256=digest(self.guest), result_sha256=digest(new))
        for key, name in (('path', str(self.victim)), ('path', '../outside/victim'), ('file', str(self.outside / 'new')),
                          ('file', '../../outside/new')):
            with self.subTest(key=key, name=name):
                self.refused('install refused: replace/manifest.json', dict(files=[dict(good, **{key: name})]))
                self.untouched()

    def test_malformed_manifest_is_a_refusal(self):
        self.manifest['files'] = [dict(stub='stubs/prog.stub')]
        self.refused('embed/manifest.json: malformed')
        self.untouched()

    def test_runtime_file_destination_symlink(self):
        (self.system / 'usr/lib/hao/libhao.dylib').symlink_to(self.victim)
        self.refused('usr/lib/hao/libhao.dylib is a symbolic link')
        self.assertTrue((self.system / 'usr/lib/hao/libhao.dylib').is_symlink())
        self.assertFalse((self.system / 'usr/libexec/hao').exists())     # checked before the first write

    def test_runtime_directory_symlink(self):
        (self.system / 'usr/libexec').symlink_to(self.outside, target_is_directory=True)
        self.refused('usr/libexec is a symbolic link')
        self.assertEqual(sorted(p.name for p in self.outside.iterdir()), ['victim'])
        self.assertFalse((self.system / 'usr/lib/hao/libhao.dylib').exists())

    def test_payload_root_symlink(self):
        (self.payload / 'root/usr/lib/hao/extra').symlink_to(self.victim)
        self.refused('root/usr/lib/hao/extra is a symbolic link')
        self.assertFalse((self.system / 'usr/lib/hao/extra').exists())

    def test_record_symlink(self):
        (self.system / 'usr/lib/hao/install.json').symlink_to(self.victim)
        self.refused('install.json is a symbolic link')
        self.assertFalse((self.system / 'usr/lib/hao/libhao.dylib').exists())   # checked before the first write
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.guest)

    def test_embed_target_through_a_symlinked_directory(self):
        (self.outside / 'bin').mkdir()
        (self.outside / 'bin/prog').write_bytes(self.guest)
        (self.system / 'usr/bin/prog').unlink(); (self.system / 'usr/bin').rmdir()
        (self.system / 'usr/bin').symlink_to(self.outside / 'bin', target_is_directory=True)
        self.refused('usr/bin is a symbolic link')
        self.assertEqual((self.outside / 'bin/prog').read_bytes(), self.guest)

    def test_embed_target_symlink_is_missing_not_followed(self):
        other = self.outside / 'prog'
        other.write_bytes(self.guest)
        (self.system / 'usr/bin/prog').unlink()
        (self.system / 'usr/bin/prog').symlink_to(other)
        self.refused('usr/bin/prog (required, not embedded)')
        self.assertEqual(other.read_bytes(), self.guest)
        self.assertEqual(json.loads((self.system / 'usr/lib/hao/install.json').read_text())['missing'], ['usr/bin/prog'])

    def test_stub_through_a_symlink(self):
        (self.payload / 'embed/stubs').rename(self.outside / 'stubs')
        (self.payload / 'embed/stubs').symlink_to(self.outside / 'stubs', target_is_directory=True)
        self.refused('stubs is a symbolic link')
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.guest)

    def test_replace_target_symlink(self):
        new = b'replacement'
        (self.payload / 'replace').mkdir()
        (self.payload / 'replace/new').write_bytes(new)
        (self.system / 'usr/bin/tool').symlink_to(self.victim)
        self.refused('usr/bin/tool is a symbolic link', dict(files=[dict(
            path='usr/bin/tool', file='new', source_sha256=digest(self.victim.read_bytes()), result_sha256=digest(new))]))

    def test_aot_directory_parent_symlink(self):
        (self.outside / 'db').mkdir()
        (self.system / 'private/var').mkdir(parents=True)
        (self.system / 'private/var/db').symlink_to(self.outside / 'db', target_is_directory=True)
        self.refused('private/var/db is a symbolic link')
        self.assertEqual(list((self.outside / 'db').iterdir()), [])

    def test_rule_is_the_release_tool_s(self):
        """hao-install.py repeats hao_release.relative (it ships without hao_release.py): same verdicts"""
        import hao_release
        install = importlib.machinery.SourceFileLoader('hao_install', str(INSTALL)).load_module()
        names = ['usr/bin/prog', 'a', 'a/b.c', 'a b/c', '.hidden/x', 'a/..b', '...', '/a', '//a', 'a/', 'a//b', './a', 'a/./b',
                 'a/.', '.', '..', '../a', 'a/..', 'a/../b', 'a\\b', 'a\0b', '', ' ', 'é/ü', 7, None, b'a', ['a']]
        for name in names:
            with self.subTest(name=name):
                try:
                    hao_release.relative(name); want = True
                except hao_release.Invalid:
                    want = False
                try:
                    install.relative(name); have = True
                except install.Unsafe:
                    have = False
                self.assertEqual(have, want)


if __name__ == '__main__':
    unittest.main()
