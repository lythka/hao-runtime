"""tools/hao_ifunit.py match: interface units against the images of an x86_64 shared cache."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
import hao_ifunit  # noqa: E402

LIB_UUID = '3d3e463264dd3015a0a8ccd98d931065'
CACHE_UUID = '5a3da575f02e33dc9b14f663050011e6'
BASE = 0x7ff800000000


def cache_file(path, image_uuid):
    """a dyld shared cache with one image (a dylib with only an LC_UUID) and no subcaches"""
    b = bytearray(0x500)
    b[0:16] = b'dyld_v1  x86_64\0'
    struct.pack_into('<II', b, 0x10, 0x200, 1)               # mappings
    b[0x58:0x68] = bytes.fromhex(CACHE_UUID)
    struct.pack_into('<II', b, 0x1c0, 0x220, 1)              # images
    struct.pack_into('<QQQ', b, 0x200, BASE, len(b), 0)      # one mapping: the whole file
    struct.pack_into('<QQQI', b, 0x220, BASE + 0x400, 0, 0, 0x300)
    name = b'/usr/lib/libfixture.dylib\0'
    b[0x300:0x300 + len(name)] = name
    struct.pack_into('<IIIIIIII', b, 0x400, 0xfeedfacf, 0x01000007, 3, 6, 1, 24, 0, 0)
    struct.pack_into('<II', b, 0x420, 0x1b, 24)
    b[0x428:0x438] = bytes.fromhex(image_uuid)
    path.write_bytes(bytes(b))


def unit(path, uuid):
    path.write_bytes(hao_ifunit.build(uuid, hao_ifunit.parse_records(['_f\tEXACT\t-\t-\t-\t-\t-\n'])))


class MatchTests(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        self.d = Path(t.name)
        self.cache = self.d / 'dyld'
        self.cache.mkdir()
        cache_file(self.cache / 'dyld_shared_cache_x86_64', LIB_UUID)
        self.units = self.d / 'if'
        self.units.mkdir()

    def run_match(self, cache=None):
        return subprocess.run([sys.executable, '-I', str(TOOLS / 'hao_ifunit.py'), 'match', str(self.units),
                               str(cache or self.cache)], capture_output=True, text=True)

    def test_matching_unit(self):
        unit(self.units / LIB_UUID, LIB_UUID)
        (self.units / 'swift.thunks').write_bytes(b'not a unit: ignored')
        r = self.run_match()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, '1 of 1 units name an image of the x86_64 cache %s\n' % CACHE_UUID.upper())
        self.assertEqual(hao_ifunit.match(str(self.units), str(self.cache / 'dyld_shared_cache_x86_64'))[1],
                         {LIB_UUID: '/usr/lib/libfixture.dylib'})

    def test_unit_of_another_library(self):
        other = 'f' * 32
        unit(self.units / LIB_UUID, LIB_UUID)
        unit(self.units / other, other)
        r = self.run_match()
        self.assertEqual(r.returncode, 1)
        self.assertIn('%s: no image of the cache has this UUID' % other, r.stdout)
        self.assertIn('1 of 2 units', r.stdout)

    def test_unit_under_another_name(self):
        unit(self.units / ('0' * 32), LIB_UUID)
        r = self.run_match()
        self.assertEqual(r.returncode, 1)
        self.assertIn('not a valid unit for this name: library UUID differs', r.stdout)

    def test_not_a_cache(self):
        (self.d / 'junk').write_bytes(b'\0' * 0x400)
        r = self.run_match(self.d / 'junk')
        self.assertEqual(r.returncode, 1)
        self.assertIn('not a dyld shared cache', r.stderr)


if __name__ == '__main__':
    unittest.main()
