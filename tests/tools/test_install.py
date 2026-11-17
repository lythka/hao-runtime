"""hao-install.py path rules: manifest names stay under SYSTEM and PAYLOAD, and
nothing is written or read through a symbolic link; the payload holds exactly
what payload-manifest.json lists and only those bytes are installed; hao stubs
that do not name their arm slice (legacy stubs) are replaced or refused.

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


def hao_stub(uuid, cli=False, lib=b'/usr/lib/hao/libhao.dylib'):
    """A thin x86_64 executable that links libhao.dylib (a hao stub; with CLI, hao's launcher: __TEXT,__hao_cli),
    without a __TEXT,__hao_slice record."""
    name = lib + b'\0' * (8 - len(lib) % 8)
    cmds = struct.pack('<II', 0x1b, 24) + bytes.fromhex(uuid)
    cmds += struct.pack('<IIIIII', 0xc, 24 + len(name), 24, 0, 0, 0) + name
    n = 2
    if cli:
        cmds += struct.pack('<II16s4Q4I', 0x19, 152, b'__TEXT', 0, 0, 0, 0, 5, 5, 1, 0)
        cmds += struct.pack('<16s16sQQ8I', b'__hao_cli', b'__TEXT', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)
        n = 3
    return struct.pack('<IIIIIIII', 0xfeedfacf, hao_fat.CPU_X86_64, 3, 2, n, len(cmds), 0, 0) + cmds + b'stub'


def write_payload_manifest(payload):
    """payload-manifest.json as tools/hao-image-payload writes it: the sha256 of every other file (no link followed)"""
    files = {}
    for base, _, names in os.walk(payload):
        for n in names:
            f = os.path.join(base, n)
            rel = os.path.relpath(f, payload)
            if rel != 'payload-manifest.json' and not os.path.islink(f):
                files[rel] = digest(Path(f).read_bytes())
    Path(payload, 'payload-manifest.json').write_text(json.dumps(dict(files=files)))


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

    def run_install(self, replace=None, after=None):
        """the installer on SYSTEM and PAYLOAD; AFTER changes the payload once its manifest is written"""
        (self.payload / 'embed/manifest.json').write_text(json.dumps(self.manifest))
        if replace is not None:
            (self.payload / 'replace').mkdir(exist_ok=True)
            (self.payload / 'replace/manifest.json').write_text(json.dumps(replace))
        write_payload_manifest(self.payload)
        if after is not None:
            after()
        p = subprocess.run([sys.executable, '-I', str(INSTALL), str(self.system), str(self.payload)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        return p.returncode, p.stdout

    def refused(self, needle, replace=None, after=None):
        """the install fails with NEEDLE in a hao: error line, not a traceback, and the outside file is intact"""
        rc, out = self.run_install(replace, after)
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

    def test_unlisted_payload_file(self):
        """a file payload-manifest.json does not list (a LaunchDaemon and its program; any file outside root/ too)"""
        def daemon():
            d = self.payload / 'root/System/Library/LaunchDaemons'
            d.mkdir(parents=True)
            (d / 'evil.plist').write_bytes(b'<plist/>')
        for name, after in (('root/System/Library/LaunchDaemons/evil.plist', daemon),
                            ('root/usr/libexec/evil', lambda: (self.payload / 'root/usr/libexec/evil').write_bytes(b'x')),
                            ('embed/stubs/extra.stub', lambda: (self.payload / 'embed/stubs/extra.stub').write_bytes(b'x'))):
            with self.subTest(name=name):
                self.refused('payload %s is not in payload-manifest.json' % name, after=after)
                self.untouched()
                self.assertFalse((self.system / 'System').exists())
                self.assertFalse((self.system / 'usr/libexec').exists())
                (self.payload / name).unlink()

    def test_changed_payload_file(self):
        """a listed file whose bytes differ from its sha256 (a runtime file, a stub)"""
        for rel in ('root/usr/libexec/hao', 'embed/stubs/prog.stub'):
            with self.subTest(rel=rel):
                self.refused('payload %s does not match its sha256' % rel,
                             after=lambda: (self.payload / rel).write_bytes(b'changed'))
                self.untouched()
                self.assertFalse((self.system / 'usr/libexec').exists())
                (self.payload / rel).write_bytes(b'launcher' if rel.startswith('root') else self.stub)

    def test_listed_file_missing(self):
        """a listed file that is missing is refused; a listed AOT cache may be (made after the first boot)"""
        self.refused('payload root/usr/libexec/hao is listed in payload-manifest.json but missing',
                     after=lambda: (self.payload / 'root/usr/libexec/hao').unlink())
        self.untouched()
        (self.payload / 'root/usr/libexec/hao').write_bytes(b'launcher')
        cache = self.payload / 'root/usr/lib/hao/aot'
        cache.mkdir()
        (cache / ('a' * 32 + '.aot')).write_bytes(b'cache')
        rc, out = self.run_install(after=lambda: (cache / ('a' * 32 + '.aot')).unlink())
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.result)

    def test_payload_modes(self):
        """a set-ID or sticky runtime file is refused; installed modes are 0755 (an execute bit) or 0644"""
        hao = self.payload / 'root/usr/libexec/hao'
        for mode in (0o4755, 0o2755, 0o1755):
            with self.subTest(mode=oct(mode)):
                self.refused('set-ID or sticky', after=lambda: hao.chmod(mode))
                self.untouched()
        hao.chmod(0o775)
        (self.payload / 'root/usr/lib/hao/libhao.dylib').chmod(0o600)
        rc, out = self.run_install()
        self.assertEqual(rc, 0, out)
        self.assertEqual(((self.system / 'usr/libexec/hao').stat().st_mode & 0o7777,
                          (self.system / 'usr/lib/hao/libhao.dylib').stat().st_mode & 0o7777), (0o755, 0o644))

    def test_hard_linked_destinations(self):
        """a runtime file is replaced by rename, so another hard link to the old one keeps its bytes; a file to
        replace (replace/) that has another hard link is not written"""
        os.link(self.victim, self.system / 'usr/lib/hao/libhao.dylib')
        new = b'replacement'
        (self.payload / 'replace').mkdir()
        (self.payload / 'replace/new').write_bytes(new)
        (self.system / 'usr/bin/tool').write_bytes(b'old tool')
        os.link(self.system / 'usr/bin/tool', self.outside / 'tool')
        rc, out = self.run_install(dict(files=[dict(path='usr/bin/tool', file='new', source_sha256=digest(b'old tool'),
                                                    result_sha256=digest(new))]))
        self.assertEqual(rc, 1, out)
        self.assertIn('usr/bin/tool (has another hard link; not replaced)', out)
        self.assertEqual(self.victim.read_bytes(), b'not the installer\'s to write')
        self.assertNotEqual((self.system / 'usr/lib/hao/libhao.dylib').stat().st_ino, self.victim.stat().st_ino)
        self.assertEqual((self.outside / 'tool').read_bytes(), b'old tool')

    def test_shared_destination_directory(self):
        """a directory the installer writes in that others can change (group-writable, or an ACL granting
        write) is refused"""
        d = self.system / 'usr/lib/hao'
        d.chmod(0o775)
        self.refused('usr/lib/hao: hao: writable by group or others')
        d.chmod(0o755)
        subprocess.run(['chmod', '+a', 'everyone allow add_file', str(d)], check=True)
        self.addCleanup(subprocess.run, ['chmod', '-N', str(d)])
        self.refused('an ACL grants write')

    def test_aot_cache_hard_link(self):
        """a file in the AOT directory with another hard link is reported, and its mode left alone"""
        aot = self.system / 'usr/lib/hao/aot'
        aot.mkdir()
        other = self.outside / 'cache'
        other.write_bytes(b'x')
        other.chmod(0o600)
        os.link(other, aot / ('b' * 32 + '.aot'))
        out = self.refused('usr/lib/hao/aot/%s.aot (has another hard link)' % ('b' * 32))
        self.assertEqual(other.stat().st_mode & 0o777, 0o600, out)

    def test_name_collisions(self):
        """names that are one file on a case-insensitive volume, or a file and a directory, are refused
        (hao_release's rule; such a payload can only be made on a case-sensitive volume)"""
        install = importlib.machinery.SourceFileLoader('hao_install', str(INSTALL)).load_module()
        for names in (['usr/lib/hao/A', 'usr/lib/hao/a'], ['usr/lib/caf\u00e9', 'usr/lib/cafe\u0301'],
                      ['usr/lib/hao/x', 'usr/lib/hao/X/y']):
            with self.subTest(names=names):
                with self.assertRaises(install.Unsafe):
                    install.check_names(names)
        install.check_names(['usr/lib/hao/a', 'usr/lib/hao/b/c'])

    def test_no_payload_manifest(self):
        self.refused('payload-manifest.json: missing or malformed',
                     after=lambda: (self.payload / 'payload-manifest.json').unlink())
        self.untouched()

    def test_legacy_stub_elsewhere(self):
        """a hao stub without a __hao_slice record anywhere in SYSTEM is an error, whatever hides it: another x86
        slice beside it, libhao named only as a string (a dlopen), a launcher marker away from usr/libexec/hao;
        the payload's own launcher is not a stub, and a file the scan cannot read is an error too"""
        arm = executable(hao_fat.CPU_ARM64, '3' * 32, b'old')
        legacy = hao_fat.combine(arm, hao_stub('6' * 32))
        launcher = hao_stub('7' * 32, cli=True) + bytes(8192)
        (self.payload / 'root/usr/libexec/hao').write_bytes(launcher)
        cases = {
            'usr/bin/old': legacy,
            'usr/bin/two': hao_fat.build([(hao_fat.CPU_ARM64, 0, arm), (hao_fat.CPU_X86_64, 3, hao_stub('6' * 32)),
                                          (hao_fat.CPU_X86_64, 8, executable(hao_fat.CPU_X86_64, '8' * 32, b'plain'))]),
            'usr/bin/dlopen': hao_fat.combine(arm, executable(hao_fat.CPU_X86_64, '9' * 32, b'/usr/lib/hao/libhao.dylib')),
            'usr/bin/cli': hao_fat.combine(arm, hao_stub('7' * 32, cli=True)),
        }
        for name, data in cases.items():
            (self.system / name).write_bytes(data)
        (self.system / 'usr/bin/closed').write_bytes(bytes(8192))
        (self.system / 'usr/bin/closed').chmod(0)
        self.addCleanup((self.system / 'usr/bin/closed').chmod, 0o644)
        out = self.refused('usr/bin/old (a hao stub without a __hao_slice record')
        record = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        self.assertEqual(sorted(record['legacy_stubs']), sorted(list(cases) + ['usr/bin/closed']), out)
        self.assertNotIn('usr/libexec/hao (', out)
        self.assertEqual(record['embedded'], 1)
        self.assertEqual((self.system / 'usr/bin/old').read_bytes(), legacy)

    def test_legacy_stub_restubbed(self):
        """a manifest program embedded with a legacy stub gets this payload's stub; one of another program is an error"""
        (self.system / 'usr/bin/prog').write_bytes(hao_fat.combine(self.guest, hao_stub('6' * 32)))
        rc, out = self.run_install()
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.result)
        record = json.loads((self.system / 'usr/lib/hao/install.json').read_text())
        self.assertEqual((record['restubbed'], record['kept_native'], record['legacy_stubs']), (['usr/bin/prog'], [], []))
        other = hao_fat.combine(executable(hao_fat.CPU_ARM64, '3' * 32, b'old'), hao_stub('6' * 32))
        (self.system / 'usr/bin/prog').write_bytes(other)
        out = self.refused('usr/bin/prog (a hao stub without a __hao_slice record')
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), other)

    def test_payload_legacy_stub_not_embedded(self):
        """the payload's own stub must name the arm slice when it is a hao stub"""
        self.stub = hao_stub('6' * 32)
        (self.payload / 'embed/stubs/prog.stub').write_bytes(self.stub)
        self.entry.update(stub_sha256=digest(self.stub), result_sha256=digest(hao_fat.combine(self.guest, self.stub)))
        self.refused("usr/bin/prog (the payload's stub is a hao stub without a __hao_slice record")
        self.assertEqual((self.system / 'usr/bin/prog').read_bytes(), self.guest)

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
