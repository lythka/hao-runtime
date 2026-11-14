"""tools/hao_catalog_gate.py on a synthetic cache: a catalog with a generic and a compiler-specialized Optional,
structs with and without libswiftCore witnesses, and the bound-input checks (generated.lock, value contracts).
    python3.14 -I -B tests/tools/test_catalog_gate.py
The real-cache run is in docs/swift-metadata.md ("Checking a cache with prespecialized metadata")."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

TOP = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOP / 'tools'))
import hao_catalog_gate as g   # noqa: E402

UUID = '11111111111111111111111111111111'
CORE_TEXT = (0x10000, 0x10000)          # libswiftCore __TEXT
UI_TEXT = (0x30000, 0x1000)             # another image's __TEXT (compiler-emitted witnesses)
SQMN, SQ_PATTERN, SQ_VWT = 0x10100, 0x10200, 0x40000
POD_WV = 0x40400                        # an exported libswiftCore table ($sBi64_WV)
DATA, FLAT = 0x50000, 0x50100
KEYS, TABLE = 0x51000, 0x52000


class Memory:
    def __init__(self):
        self.b = {}

    def put(self, a, data):
        for i, x in enumerate(data): self.b[a + i] = x

    def q(self, a, v): self.put(a, struct.pack('<Q', v))

    def u32(self, a, v): self.put(a, struct.pack('<I', v & 0xffffffff))

    def read(self, a, n):
        out = bytearray()
        for i in range(n):
            if a + i not in self.b: break
            out.append(self.b[a + i])
        return bytes(out)


def generic(slot): return CORE_TEXT[0] + 0x1000 + 16 * slot


def pointer_table(mem, at, rows, cap=8):
    """a dyld Swift pointer hash table at AT (offsets from cache base 0) holding ROWS [(key words, metadata)]"""
    rounded = 4
    mem.put(at, struct.pack('<6I', cap, len(rows), 64 - (cap.bit_length() - 1), 0, 0xffffffff, rounded))
    mem.put(at + 24, bytes(1056 - 24 + rounded + cap))
    values = 1056 + rounded + 5 * cap
    keys = values + 16 * len(rows)
    offsets = [0xffffffff] * cap
    for i, (key, m) in enumerate(rows):
        offsets[i] = values + 16 * i
        mem.put(at + values + 16 * i, struct.pack('<IIQ', keys, len(key), m))
        mem.put(at + keys, struct.pack('<%dQ' % len(key), *key))
        keys += 8 * len(key)
    mem.put(at + 1056 + rounded + cap, struct.pack('<%dI' % cap, *offsets))


def build(optional_slot2=None, contract_cache=UUID, anchor=b'\x90' * 16, table_only=False, flat_only=False):
    mem = Memory()
    # Optional's descriptor: generic enum; +32 relative pointer to its pattern; pattern +12 to its value witnesses.
    mem.u32(SQMN, 0x800f2)
    mem.u32(SQMN + 32, SQ_PATTERN - (SQMN + 32))
    mem.u32(SQ_PATTERN + 12, SQ_VWT - (SQ_PATTERN + 12))
    for s in g.OPTIONAL_SLOTS: mem.q(SQ_VWT + 8 * s, generic(s))
    for s in range(8): mem.q(POD_WV + 8 * s, CORE_TEXT[0] + 0x2000 + 16 * s)
    entries = []

    def metadata(at, kind, vwt, desc=0):
        mem.q(at - 8, vwt)
        mem.q(at, kind)
        mem.q(at + 8, desc)
        return at

    # 1: Int? with the generic witnesses (its own table, the same functions)
    own = 0x41000
    for s in g.OPTIONAL_SLOTS: mem.q(own + 8 * s, generic(s))
    entries.append(('$sSiSg', metadata(0x60010, g.KIND_OPTIONAL, own, SQMN)))
    # 2: String? whose slot 2 the compiler specialized (outside libswiftCore) when asked
    spec = 0x42000
    for s in g.OPTIONAL_SLOTS: mem.q(spec + 8 * s, generic(s))
    if optional_slot2 is not None: mem.q(spec + 16, optional_slot2)
    entries.append(('$sSSSg', metadata(0x60110, g.KIND_OPTIONAL, spec, SQMN)))
    # 3: a POD struct with libswiftCore's table; 4: a struct with its own witnesses in another image
    entries.append(('$s4Test3PODVySiG', metadata(0x60210, g.KIND_STRUCT, POD_WV)))
    miss = 0x43000
    for s in range(8): mem.q(miss + 8 * s, UI_TEXT[0] + 16 * s)
    entries.append(('$s4Test4ViewVySiG', metadata(0x60310, g.KIND_STRUCT, miss)))
    # 5: a class (value witnesses not compared)
    mem.q(0x60410, 0x7ff812345678)
    entries.append(('$s4Test5ModelCySiG', 0x60410))
    # the catalog: LibPrespecializedData 1.5, flat pointer-keyed map, one key symbol per entry
    mem.u32(DATA, 1)
    mem.u32(DATA + 4, 5)
    mem.q(DATA + 8, 0)
    # (libswiftCore searches the +24 table; the flat list names the keys). table_only: entry 2 (String?) is missing
    # from the flat list, as in a rebuilt cache that adds table rows only; flat_only: missing from the table.
    mem.q(DATA + 24, TABLE)
    flat = [e for i, e in enumerate(entries) if not (table_only and i == 1)]
    mem.q(FLAT, len(flat))
    exports_pre = {'__swift_prespecializationsData': DATA, '__swift_prespecializedPointerKeyedMetadataMap': FLAT}
    words = lambda i: (0x80000 + 0x10 * i, 0x81000)
    for i, (name, m) in enumerate(flat):
        key = KEYS + 32 * i
        exports_pre['__metadataMapKey_' + name] = key
        mem.q(key, 2)
        for j, w in enumerate(words(entries.index((name, m)))): mem.q(key + 8 + 8 * j, w)
        mem.q(FLAT + 8 + 16 * i, key)
        mem.q(FLAT + 16 + 16 * i, m)
    pointer_table(mem, TABLE, [(words(i), m) for i, (_, m) in enumerate(entries) if not (flat_only and i == 1)])
    # a value contract's anchor in the Foundation image
    foundation = 0x70000
    mem.put(foundation, bytes(0x1000))
    mem.put(foundation + 0x100, anchor)
    images = g.Images({
        g.SWIFTCORE: (CORE_TEXT[0], {'_$sSqMn': SQMN, '_$sBi64_WV': POD_WV},
                      [('__TEXT', CORE_TEXT[0], CORE_TEXT[1]), ('__DATA_CONST', 0x40000, 0x8000)]),
        g.PRESPECIALIZED: (0x20000, exports_pre, [('__TEXT', 0x20000, 0x1000), ('__DATA_CONST', 0x50000, 0x20000)]),
        '/System/Library/Frameworks/SwiftUICore.framework/SwiftUICore': (UI_TEXT[0], {}, [('__TEXT',) + UI_TEXT]),
        '/System/Library/Frameworks/Foundation.framework/Foundation': (foundation, {}, [('__TEXT', foundation, 0x1000)]),
    }, {'/System/Library/Frameworks/Foundation.framework/Foundation': 'AB' * 16})
    v = g.View(mem.read, UUID, images, 0, 0)
    contract = {'targets': {'x86_64': {'cache_uuid': contract_cache.lower(), 'image_uuid': 'ab' * 16, 'anchors': [
        {'role': 'body', 'address': foundation + 0x100, 'size': 16, 'sha256': hashlib.sha256(b'\x90' * 16).hexdigest()}]}}}
    return v, contract


class CatalogGate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def inputs(self, contract, lock_uuid=UUID):
        (self.dir / 'contracts').mkdir(exist_ok=True)
        (self.dir / 'contracts' / 'optional-data-TEST.json').write_text(json.dumps(contract))
        (self.dir / 'generated.lock').write_text(json.dumps({'lock_version': 1, 'files': [
            {'path': 'src/a_gen.inc', 'input_kind': 'cache', 'cache_uuid': lock_uuid},
            {'path': 'src/b_gen.inc', 'input_kind': 'sdk', 'sdk_build': '26B118'}]}))
        return str(self.dir / 'generated.lock'), str(self.dir / 'contracts')

    def test_catalog_read(self):
        v, _ = build()
        names = [n for n, _ in g.catalog_entries(v)]
        self.assertEqual(names, ['$sSiSg', '$sSSSg', '$s4Test3PODVySiG', '$s4Test4ViewVySiG', '$s4Test5ModelCySiG'])

    def test_catalog_sources(self):
        v, _ = build()
        g.catalog_entries(v)
        self.assertEqual(v.catalog_sources, {'string-keyed': 0, 'pointer-keyed table': 5, 'flat list': 5,
                                             'table only': 0, 'flat only': 0, 'same key, other metadata': 0})

    def test_table_only_entry_is_checked(self):
        v, _ = build(optional_slot2=UI_TEXT[0] + 0x80, table_only=True)
        entries = g.catalog_entries(v)
        self.assertEqual(v.catalog_sources['table only'], 1)
        self.assertIn((None, 0x60110), entries)
        ok, _, bad = g.gate_optional(v, entries)
        self.assertFalse(ok)
        self.assertEqual(list(bad), ['0x60110'])

    def test_flat_only_entry_is_checked(self):
        v, _ = build(optional_slot2=UI_TEXT[0] + 0x80, flat_only=True)
        entries = g.catalog_entries(v)
        self.assertEqual(v.catalog_sources['flat only'], 1)
        ok, _, bad = g.gate_optional(v, entries)
        self.assertEqual(list(bad), ['$sSSSg'])

    def test_generic_optional_witnesses_pass(self):
        v, _ = build()
        ok, lines, bad = g.gate_optional(v, g.catalog_entries(v))
        self.assertTrue(ok, lines)
        self.assertIn('2 prespecialized Optionals, 0 with other witnesses', lines[0])

    def test_specialized_optional_witness_fails_closed(self):
        v, _ = build(optional_slot2=UI_TEXT[0] + 0x80)
        ok, lines, bad = g.gate_optional(v, g.catalog_entries(v))
        self.assertFalse(ok)
        self.assertEqual(list(bad), ['$sSSSg'])
        self.assertEqual(list(bad['$sSSSg']), [2])
        self.assertIn('SwiftUICore', '\n'.join(lines))

    def test_fast_path_count(self):
        v, _ = build()
        lines, stat = g.gate_fast_paths(v, g.catalog_entries(v))
        self.assertEqual(stat['miss'], 1, lines)
        self.assertEqual(stat['struct: libswiftCore table $sBi64_WV'], 1)
        self.assertEqual(stat['optional: own table, witnesses in libswiftCore'], 2)
        self.assertEqual(stat['class (no value witnesses compared)'], 1)

    def test_bound_inputs_pass(self):
        v, contract = build()
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract))
        self.assertTrue(ok, lines)

    def test_lock_for_another_cache_fails(self):
        v, contract = build()
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract, lock_uuid='2' * 32))
        self.assertFalse(ok)
        self.assertIn('FAIL src/a_gen.inc', '\n'.join(lines))
        self.assertNotIn('b_gen', '\n'.join(lines))

    def test_contract_for_another_cache_fails_even_when_bytes_match(self):
        v, contract = build(contract_cache='3' * 32)
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract))
        self.assertFalse(ok)
        self.assertIn('anchor hashes still match', '\n'.join(lines))

    def test_contract_code_changed_fails(self):
        v, contract = build(anchor=b'\xcc' * 16)
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract))
        self.assertFalse(ok)
        self.assertIn('anchor bytes changed: body', '\n'.join(lines))

    def test_changed_code_under_same_uuid_fails(self):
        v, contract = build()
        base, _ = build(anchor=b'\x91' * 16)   # the base cache: same image UUID, other __TEXT bytes
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=base)
        self.assertFalse(ok)
        self.assertIn('changed under the same UUID', '\n'.join(lines))


    def rebuilt_base(self, move=0x100000, export_shift=0):
        """the base cache as the family looked before a rebuild: another cache UUID, other code bytes, Foundation
        at another address with its export EXPORT_SHIFT bytes away from where the rebuilt one has it"""
        base, _ = build(anchor=b'\x91' * 16)
        base.uuid = '2' * 32
        f = '/System/Library/Frameworks/Foundation.framework/Foundation'
        a, ex, segs = base.images.entries[f]
        base.images.entries[f] = (a + move, {'_NSFoo': a + move + 0x100 + export_shift},
                                  [(n, sa + move, ss) for n, sa, ss in segs])
        return base

    def rebuilt(self):
        v, contract = build()
        f = '/System/Library/Frameworks/Foundation.framework/Foundation'
        a, _, segs = v.images.entries[f]
        v.images.entries[f] = (a, {'_NSFoo': a + 0x100}, segs)
        return v, contract

    def test_rebuilt_family_relinked_image_passes(self):
        v, contract = self.rebuilt()
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=self.rebuilt_base())
        self.assertTrue(ok, lines)
        self.assertIn('rebuilt family: 1 images checked were placed again', '\n'.join(lines))

    def test_rebuilt_family_moved_export_fails(self):
        v, contract = self.rebuilt()
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=self.rebuilt_base(export_shift=0x10))
        self.assertFalse(ok)
        self.assertIn('changed under the same UUID', '\n'.join(lines))

    def accepted(self, before, after, address=0x70100):
        r = self.dir / 'recipe.json'
        r.write_text(json.dumps({'files': [{'file': 'main', 'edits': [
            {'id': 'edit-1', 'address': hex(address), 'before': before.hex(), 'after': after.hex()}]}]}))
        return g.load_accepted([str(r)])

    def test_changed_code_inside_accepted_edit_passes(self):
        v, contract = build()
        base, _ = build(anchor=b'\x91' * 16)
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=base,
                                        accepted=self.accepted(b'\x91' * 16, b'\x90' * 16))
        self.assertTrue(ok, lines)
        self.assertIn('changed only by accepted edits (edit-1)', '\n'.join(lines))

    def test_change_outside_accepted_edit_fails(self):
        v, contract = build()
        base, _ = build(anchor=b'\x91' * 16)
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=base,   # covers 8 of the 16 changed bytes
                                        accepted=self.accepted(b'\x91' * 8, b'\x90' * 8))
        self.assertFalse(ok)
        self.assertIn('changed under the same UUID', '\n'.join(lines))

    def test_accepted_edit_with_other_before_bytes_fails(self):
        v, contract = build()
        base, _ = build(anchor=b'\x91' * 16)
        ok, lines = g.gate_bound_inputs(v, *self.inputs(contract), base=base,
                                        accepted=self.accepted(b'\x92' * 16, b'\x90' * 16))
        self.assertFalse(ok)


if __name__ == '__main__':
    unittest.main()
