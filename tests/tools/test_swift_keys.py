"""tools/hao_swift_keys.py on synthetic logs: hao-swift-key and hao-swift-audit records among other lines, the
walk table's keys from a generated-table excerpt, a catalog name list, and the TSV and specs outputs.
    python3.14 -I -B tests/tools/test_swift_keys.py
The records' producer is src/swiftvalue/Facts.c (key_met) and src/swiftvalue.c (audit_record)."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOP / 'tools'))
import hao_swift_keys as k   # noqa: E402

CACHE = 'C1C7D64C41C73219BB29A98228FB1D3E'


def key(process, name, row, where):
    return 'hao-swift-key ' + json.dumps({'v': 1, 'process': process, 'key': name, 'row': row, 'where': where, 'kind': 512,
                                          'size': 8, 'stride': 8, 'flags': 7, 'xi': 0, 'x86_cache': CACHE}) + '\n'


def audit(process, name, reason):
    return 'hao-swift-audit ' + json.dumps({'v': 1, 'process': process, 'reason': reason, 'class': 'walk-limit', 'type': name,
                                            'kind': 512, 'x86_cache': CACHE}) + '\n'


TABLE = '''/* excerpt */
#if defined(SWIFTWALK_TYPES)
    {"SiSg", SWIFTWALK_SAME, 9, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, 0, 0, 0, 0, 0, 0, 0}, /* Int? */
    {"SSSg", SWIFTWALK_SAME, 16, 16, 8, 0x7, 0, 0, SWIFTWALK_XI_NONE, 0, 0, 0, 0, 0, 0, 0},
#endif
#if defined(SWIFTWALK_IMAGES)
    {0, 0, "x", "y", "z", "w"},
#endif
'''


class SwiftKeys(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        log = self.d / 'console.log'
        log.write_text('hao[ControlCenter:12]: something else\n' +
                       key('ControlCenter', 'SiSg', 'same', 'cache') +
                       key('Dock', 'SiSg', 'same', 'cache') +
                       '@12.5 ' + key('ControlCenter', '7SwiftUI4TextVSg', 'none', 'heap') +
                       key('ControlCenter', '12ControlCenter4ItemV', 'none', 'guest') +
                       key('Dock', 'SSSg', 'other-cache', 'heap') +
                       key('Dock', 'Sb', 'layout', 'cache') +
                       audit('ControlCenter', '7SwiftUI4TextVSg', 'type graph past the walk\'s bounds') +
                       audit('Dock', '7SwiftUI5ColorV', 'unresolved active representation') +
                       'hao-swift-key {not json}\n' +
                       'hao-swift-key ' + json.dumps({'v': 2, 'key': 'future'}) + '\n')
        self.log = str(log)
        (self.d / 'walk.inc').write_text(TABLE)
        (self.d / 'names').write_text('# catalog\n7SwiftUI4TextVSg\nSaySiG\n')

    def tearDown(self):
        self.tmp.cleanup()

    def test_records(self):
        keys, n = k.read_records([self.log])
        self.assertEqual(n, 8)
        self.assertEqual(sorted(keys['SiSg']['processes']), ['ControlCenter', 'Dock'])
        self.assertEqual(keys['7SwiftUI4TextVSg']['reasons'], {"type graph past the walk's bounds"})
        self.assertEqual(k.answer(keys['7SwiftUI5ColorV']), 'unrecorded')
        self.assertNotIn('future', keys)

    def test_report(self):
        keys, _ = k.read_records([self.log])
        r = k.report(keys, k.walk_table_keys(str(self.d / 'walk.inc')),
                     {'7SwiftUI4TextVSg', 'SaySiG'})
        self.assertEqual(r['walk_coverage'], [1, 4])   # SiSg has a row; Text?, Item and Bool do not
        self.assertEqual(r['other_cache_listed'], ['SSSg'])
        self.assertEqual(r['uncertified'], ['7SwiftUI4TextVSg', '7SwiftUI5ColorV'])
        self.assertEqual(r['heap'], 2)
        self.assertEqual(r['heap_in_catalog'], ['7SwiftUI4TextVSg'])
        self.assertEqual(r['heap_not_in_catalog'], 1)

    def test_outputs(self):
        tsv, specs, js = (str(self.d / n) for n in ('k.tsv', 'k.specs', 'k.json'))
        rc = k.main([self.log, '--walk-table', str(self.d / 'walk.inc'), '--catalog-names', str(self.d / 'names'),
                     '--tsv', tsv, '--specs', specs, '--json', js])
        self.assertEqual(rc, 0)
        rows = [l.split('\t') for l in Path(tsv).read_text().splitlines()]
        self.assertEqual(rows[0][0], 'key')
        self.assertIn(['SiSg', 'same', 'cache', 'ControlCenter,Dock', '', CACHE], rows)
        s = Path(specs).read_text().splitlines()
        self.assertTrue(s[1].startswith('mangled:7SwiftUI4TextVSg  # ControlCenter; type graph'))
        self.assertIn('# guest-defined: mangled:12ControlCenter4ItemV  # ControlCenter', s)
        self.assertNotIn('SiSg', ''.join(s))
        self.assertEqual(json.loads(Path(js).read_text())['walk_coverage'], [1, 4])
        self.assertEqual(k.main([str(self.d / 'missing')]), 2)


if __name__ == '__main__':
    unittest.main()
