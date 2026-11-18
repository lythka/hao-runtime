"""hao-install.py --overlay: what installing a payload over an installed system would write, as the pack overlay the
live-patch backend applies (manifest.json format 1 + files/), with nothing under the system written; and
hao-install.py --installed, the installed payload's identity.

  python3 -I tests/tools/test_install_overlay.py        (tests/tools/run.sh runs it)

SYSTEM and PAYLOAD are synthetic (test_install_live.py's fixtures). Applying an overlay is modelled here by copying
its files over a copy of SYSTEM; the result must equal what hao-runtime-on-mac --install-live writes.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('test_install_live', HERE / 'test_install_live.py')
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)
hao_fat, digest, executable, make_payload, tree = L.hao_fat, L.digest, L.executable, L.make_payload, L.tree
INSTALL, BUILD, CACHE = L.INSTALL, L.BUILD, L.CACHE


def load_overlay(folder):
    """The checks tools/hao livepatch.load_overlay makes (format 1, phase, every file present with its sha256)."""
    m = json.loads((folder / 'manifest.json').read_text())
    assert m['format'] == 1 and m['phase'] in ('installer', 'live-capable') and isinstance(m['files'], dict), m
    for path, meta in m['files'].items():
        assert not path.startswith('/') and '..' not in path.split('/'), path
        assert digest((folder / 'files' / path).read_bytes()) == meta['sha256'], path
        assert set(meta) >= {'sha256', 'mode', 'owner', 'flags'}, meta
    return m


def apply(overlay, root):
    """What the live patch does with an overlay, without attributes: each file replaced by rename; the "remove"
    paths removed."""
    for path in overlay['files']:
        t = root / path
        t.parent.mkdir(parents=True, exist_ok=True)
        tmp = t.parent / ('.new.' + t.name)
        shutil.copyfile(Path(overlay['dir']) / 'files' / path, tmp)
        os.replace(tmp, t)
    for path in overlay.get('remove', []):
        (root / path).unlink()


class InstallOverlay(unittest.TestCase):
    # test_install_live.py's installed SYSTEM (payload p1) and helpers; its tests run there only
    setUp, live, changed = L.InstallLive.setUp, L.InstallLive.live, L.InstallLive.changed

    def overlay(self, payload, *extra, system=None):
        out = self.base / ('ov%d' % len(list(self.base.glob('ov*'))))
        p = subprocess.run([sys.executable, '-I', str(INSTALL), '--overlay', str(out), str(system or self.system),
                            str(payload), *map(str, extra)], capture_output=True, text=True, timeout=60)
        self.assertNotIn('Traceback', p.stdout + p.stderr)
        m = None
        if p.returncode == 0:
            m = load_overlay(out)
            m['dir'] = str(out)
            self.assertEqual(os.stat(out).st_mode & 0o777, 0o700)
        else:
            self.assertFalse(out.exists(), 'a refused overlay leaves no folder')
        return p.returncode, p.stdout + p.stderr, m

    def test_libhao_only(self):
        """libhao changed: the overlay holds libhao.dylib and install.json, lists the old libhao's AOT cache to
        remove, and SYSTEM is untouched"""
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out, m = self.overlay(self.p2)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.changed(), [])
        self.assertEqual(sorted(m['files']), ['usr/lib/hao/install.json', 'usr/lib/hao/libhao.dylib'])
        self.assertEqual(m['pack'], 'hao_runtime')
        self.assertEqual(m['phase'], 'live-capable')
        self.assertEqual(m['remove'], ['usr/lib/hao/aot/%s.aot' % self.aot])
        lib = m['files']['usr/lib/hao/libhao.dylib']
        self.assertEqual((lib['mode'], lib['owner'], lib['flags'], lib['sha256']),
                         ('0644', 'root:wheel', [], digest((self.p2 / 'root/usr/lib/hao/libhao.dylib').read_bytes())))
        rec = json.loads((Path(m['dir']) / 'files/usr/lib/hao/install.json').read_text())
        self.assertEqual((rec['payload']['commit'], rec['live']['previous']['commit'], rec['live']['via'],
                          rec['aot_caches']['libhao_uuid'], rec['errors'], rec['embedded']),
                         ('c2', 'c1', 'overlay', '3' * 32, [], 1))
        r = m['hao_runtime']
        self.assertEqual((r['root'], r['embedded'], r['restubbed'], r['up_to_date'], r['libhao_uuid']),
                         (['usr/lib/hao/libhao.dylib'], [], [], False, '3' * 32))

    def test_same_as_install_live(self):
        """a restub and a libhao change: SYSTEM with the overlay applied equals SYSTEM after --install-live (bytes of
        every file; install.json apart from how it was made); then the same payload gives an empty overlay"""
        prog = self.system / 'usr/bin/prog'
        with open(prog, 'r+b') as f:
            f.write(hao_fat.combine(self.guest, L.hao_stub('6' * 32))); f.truncate()
        os.chmod(prog, 0o751)
        self.before = tree(self.system)
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out, m = self.overlay(self.p2)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.changed(), [])
        self.assertEqual(sorted(m['files']), ['usr/bin/prog', 'usr/lib/hao/install.json', 'usr/lib/hao/libhao.dylib'])
        e = m['files']['usr/bin/prog']
        st = prog.stat()
        self.assertEqual((e['sha256'], e['mode'], e['owner']),
                         (digest(hao_fat.combine(self.guest, self.stub)), '0751', '%d:%d' % (st.st_uid, st.st_gid)))
        self.assertEqual(m['hao_runtime']['restubbed'], ['usr/bin/prog'])
        live = self.base / 'live'
        shutil.copytree(self.system, live, symlinks=True)
        rc, out = self.live(self.p2, '--system', live)
        self.assertEqual(rc, 0, out)
        apply(m, self.system)
        a, b = tree(self.system), tree(live)
        self.assertEqual(sorted(a), sorted(b))
        rec = 'usr/lib/hao/install.json'
        self.assertEqual({k: v[2] for k, v in a.items() if k != rec}, {k: v[2] for k, v in b.items() if k != rec})
        ra, rb = (json.loads((d / rec).read_text()) for d in (self.system, live))
        self.assertEqual(ra['live'].pop('via'), 'overlay')
        self.assertEqual(ra, rb)
        rc, out, m = self.overlay(self.p2)
        self.assertEqual(rc, 0, out)
        self.assertEqual((m['files'], m['remove'], m['hao_runtime']['up_to_date']), ({}, [], True))
        self.assertIn('; up to date', out)

    def test_flags_kept(self):
        """a runtime file's chflags (the image may set restricted, a system flag) are carried into the overlay, so the
        live patch does not clear them; a user flag stands in for it here"""
        launcher = self.system / 'usr/libexec/hao'
        launcher.write_bytes(b'old launcher')
        os.chflags(launcher, 0x8000)   # hidden
        self.addCleanup(lambda: os.chflags(launcher, 0))
        make_payload(self.p2, '1' * 32, self.stub, self.guest, commit='c2')
        rc, out, m = self.overlay(self.p2)
        self.assertEqual(rc, 0, out)
        self.assertEqual(m['files']['usr/libexec/hao']['flags'], ['hidden'])

    def test_json(self):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        rc, out, m = self.overlay(self.p2, '--json')
        self.assertEqual(rc, 0, out)
        r = json.loads(out)
        self.assertEqual((r['files'], r['payload']['commit'], r['previous']['commit'], r['overlay']),
                         (2, 'c2', 'c1', m['dir']))
        self.assertEqual(r['bytes'], sum(e['size'] for e in m['files'].values()))

    def refused(self, needle, **meta):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, **meta)
        rc, out, _ = self.overlay(self.p2)
        self.assertEqual(rc, 1, out)
        self.assertIn('hao: error: overlay refused: ', out)
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

    def test_embed_error(self):
        (self.system / 'usr/bin/prog').write_bytes(executable(hao_fat.CPU_ARM64, '9' * 32, b'other'))
        self.before = tree(self.system)
        self.refused('usr/bin/prog (required, not embedded)', commit='c2')

    def test_hard_link(self):
        """a program to restub with another hard link: the live patch's rename would leave the other link with the
        old bytes, so the overlay is refused"""
        prog = self.system / 'usr/bin/prog'
        with open(prog, 'r+b') as f:
            f.write(hao_fat.combine(self.guest, L.hao_stub('6' * 32))); f.truncate()
        os.link(prog, self.system / 'usr/bin/prog2')
        self.before = tree(self.system)
        self.refused('usr/bin/prog (has another hard link; a live update would split it)', commit='c2')

    def test_out_exists(self):
        make_payload(self.p2, '3' * 32, self.stub, self.guest, commit='c2')
        (self.base / 'ov0').mkdir()
        rc, out, _ = self.overlay(self.p2)   # names ov1, which does not exist
        self.assertEqual(rc, 0, out)
        p = subprocess.run([sys.executable, '-I', str(INSTALL), '--overlay', str(self.base / 'ov0'), str(self.system),
                            str(self.p2)], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn('exists: the overlay goes into a new folder', p.stderr)

    def test_installed(self):
        """--installed: hao-install.py records the payload's identity in install.json; --installed prints it with
        the libhao UUID and the system's os_build and cache"""
        p = subprocess.run([sys.executable, '-I', str(INSTALL), '--installed', str(self.system)], capture_output=True,
                           text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        r = json.loads(p.stdout)
        self.assertEqual((r['installed'], r['payload']['commit'], r['payload']['os_build'], r['libhao_uuid'],
                          r['os_build'], r['cache_uuid']), (True, 'c1', BUILD, '1' * 32, BUILD, CACHE))
        (self.system / 'usr/lib/hao/install.json').unlink()
        p = subprocess.run([sys.executable, '-I', str(INSTALL), '--installed', str(self.system)], capture_output=True,
                           text=True, timeout=60)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertFalse(json.loads(p.stdout)['installed'])


if __name__ == '__main__':
    unittest.main()
