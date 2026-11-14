#!/usr/bin/env python3
"""hao_catalog_gate.py CACHE [--base-cache BASE] [--units DIR] [--lock FILE] [--contracts DIR] [--json OUT]

The checks an x86_64 shared cache with prespecialized Swift metadata (a
libswiftPrespecialized catalog) must pass before hao runs on it
(docs/swift-metadata.md, "Requirements a cache with prespecialized metadata
must meet"). CACHE is the main dyld_shared_cache_x86_64 file or its
directory; subcaches are read next to it.

1. Optional witnesses (fails closed). Every prespecialized Optional in the
   catalog must use libswiftCore's generic Optional value witnesses, the ones
   every run-time Optional instantiation shares and src/swiftvalue.c
   recognizes for the outer-nil repair. They are read from the instantiation
   pattern of `$sSqMn` in the same cache. Slots 0-7 and 11-13 are compared
   (size, stride and flags are per type). Any other witness is listed with the
   image holding it; swiftvalue has to learn those before the cache is used.
2. Pointer-compared fast paths (a count, never a failure). Value types in the
   catalog whose witness table is not one of libswiftCore's exported tables and
   whose witness functions are not all inside libswiftCore's __TEXT cannot
   match the swiftvalue checks that compare with libswiftCore symbols; they
   take the slower general path.
3. Inputs bound to the cache's code (fails closed):
   - every generated.lock table of input kind "cache" must name CACHE's UUID;
   - every value contract (tools/value-contracts/*.json) whose x86_64 target
     image is in CACHE must name CACHE's UUID, and each of its anchors' code
     bytes must still hash to the pinned sha256;
   - with --units, every interface unit must match an image of CACHE
     (tools/hao_ifunit.py match);
   - with --base-cache (the cache the inputs were made for), images whose
     LC_UUID is unchanged but whose __TEXT bytes differ are listed, and a unit
     or contract bound to one of them fails: same UUID, different code.

Exit status: 0 all gates pass, 1 a gate fails, 2 the cache cannot be read.
Catalog layout: LibPrespecializedData (swift/include/swift/Runtime/
LibPrespecialized.h) versions 1.x, read through the flat
`_swift_prespecializedPointerKeyedMetadataMap` (count, then key/metadata
pairs) and the string-keyed map; slid pointers decoded from the cache's slide
info (version 2, the x86_64 format; anything else is refused)."""
import argparse
import bisect
import collections
import glob
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PRESPECIALIZED = '/usr/lib/libswiftPrespecialized.dylib'
SWIFTCORE = '/usr/lib/swift/libswiftCore.dylib'
X86_CACHE_FILE = 'dyld_shared_cache_x86_64'
OPTIONAL_SLOTS = (0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13)
KIND_STRUCT, KIND_ENUM, KIND_OPTIONAL = 0x200, 0x201, 0x202
KIND_NAMES = {0x200: 'struct', 0x201: 'enum', 0x202: 'optional', 0x300: 'foreign-class', 0x301: 'opaque',
              0x302: 'tuple', 0x303: 'existential', 0x304: 'metatype', 0x305: 'objc-wrapper'}


class Bad(Exception):
    """the cache or the catalog cannot be read as expected"""


class Images:
    """images by install name: base address, exports {name: address}, segments [(name, addr, size)]"""

    def __init__(self, entries, uuids=None):
        self.entries = entries              # path -> (base, {export: addr}, [(seg, addr, size)])
        self.uuids = uuids or {}            # path -> LC_UUID (hex)

    def has(self, path): return path in self.entries

    def all_paths(self): return list(self.entries)

    def uuid(self, path): return self.uuids.get(path)

    def base(self, path): return self.entries[path][0]

    def exports(self, path): return self.entries[path][1]

    def segment(self, path, name):
        for n, a, s in self.entries[path][2]:
            if n == name: return a, a + s
        raise Bad('%s: no %s segment' % (path, name))

    def owner(self, addr):
        """install name of the image whose segments hold ADDR, or None"""
        for path, (_, _, segs) in self.entries.items():
            if any(a <= addr < a + s for _, a, s in segs): return path
        return None


class View:
    """what the gates read: 8-, 4-byte words and bytes by address, slid pointers decoded, and the images"""

    def __init__(self, read, uuid, images, delta_mask, value_add):
        self.read, self.uuid, self.images = read, uuid, images
        self.mask, self.add = delta_mask, value_add

    def q(self, a):
        b = bytes(self.read(a, 8))
        if len(b) != 8: raise Bad('read past the cache at %#x' % a)
        return struct.unpack('<Q', b)[0]

    def u32(self, a):
        b = bytes(self.read(a, 4))
        if len(b) != 4: raise Bad('read past the cache at %#x' % a)
        return struct.unpack('<I', b)[0]

    def i32(self, a):
        v = self.u32(a)
        return v - (1 << 32) if v & 0x80000000 else v

    def ptr(self, a):
        """the slid pointer stored at A (slide info v2: delta bits masked, value_add added); 0 for null"""
        r = self.q(a) & ~self.mask
        return r + self.add if r else 0

    def rel(self, a):
        """a direct relative pointer at A"""
        return a + self.i32(a)

    def cstr(self, a, limit=4096):
        b = bytes(self.read(a, limit))
        return b.split(b'\0', 1)[0].decode('latin1')


def open_cache(path):
    """View of the x86_64 shared cache at PATH (file or directory)"""
    import hao_macho
    if os.path.isdir(path): path = os.path.join(path, X86_CACHE_FILE)
    try:
        c = hao_macho.Cache(path)
    except (OSError, ValueError) as e:
        raise Bad('%s: %s' % (path, e))
    magic = bytes(c.files[0][:16]).split(b'\0')[0].decode('latin1')
    if magic.split()[-1:] not in (['x86_64'], ['x86_64h']):
        raise Bad('%s: not an x86_64 shared cache (%s)' % (path, magic.strip()))
    slide = set()
    for mm in c.files:
        mo, mc = struct.unpack_from('<II', mm, 0x138)
        for k in range(mc):
            _, _, _, so, ss, _, _, _ = struct.unpack_from('<QQQQQQII', mm, mo + 56 * k)
            if not ss: continue
            ver = struct.unpack_from('<I', mm, so)[0]
            if ver != 2: raise Bad('%s: slide info version %d (only 2 is read)' % (path, ver))
            dm, va = struct.unpack_from('<QQ', mm, so + 24)
            slide.add((dm, va))
    if len(slide) != 1: raise Bad('%s: %d different slide encodings' % (path, len(slide)))
    dm, va = slide.pop()

    class LazyImages(Images):
        def __init__(self):
            self.entries = {}

        def _load(self, p):
            if p not in self.entries:
                im = c.image(p)
                if im is None: raise Bad('%s: no image %s' % (path, p))
                ex = {n: im.base + v[2] for n, v in im.exports().items() if v[0] == 'def'}
                self.entries[p] = (im.base, ex, [(n, vm, vs) for n, vm, vs, _, _ in im.segs])
            return self.entries[p]

        def base(self, p): return self._load(p)[0]

        def exports(self, p): return self._load(p)[1]

        def segment(self, p, name):
            self._load(p)
            return Images.segment(self, p, name)

        def owner(self, addr):
            i = bisect.bisect_right(starts, addr) - 1
            for p in (paths[i], paths[i - 1]) if i > 0 else (paths[i],) if i == 0 else ():
                if any(a <= addr < a + s for _, a, s in self._load(p)[2]): return p
            for p in self.entries:   # segments far from the header (__DATA_CONST, __LINKEDIT)
                if any(a <= addr < a + s for _, a, s in self.entries[p][2]): return p
            return None

        def has(self, p): return p in c.addr_of

        def all_paths(self): return [p for p, _ in c.paths]

        def uuid(self, p):
            try:
                im = c.image(p)
            except ValueError:
                return None
            return im.uuid if im is not None else None

    order = sorted((a, p) for p, a in c.paths)
    starts, paths = [a for a, _ in order], [p for _, p in order]
    v = View(c.read_vm, c.uuid, LazyImages(), dm, va)
    v.path = path
    return v


# ---------------------------------------------------------------- the catalog

def catalog_entries(v):
    """[(name or None, metadata address)] of every prespecialized metadata in the catalog"""
    if not v.images.has(PRESPECIALIZED): return []
    ex = v.images.exports(PRESPECIALIZED)
    data = ex.get('__swift_prespecializationsData')
    if data is None: raise Bad('%s exports no _swift_prespecializationsData' % PRESPECIALIZED)
    major, minor = v.u32(data), v.u32(data + 4)
    if major != 1: raise Bad('catalog version %d.%d (only 1.x is read)' % (major, minor))
    names = {a: n[len('__metadataMapKey_'):] for n, a in ex.items() if n.startswith('__metadataMapKey_')}
    out = []
    smap = v.ptr(data + 8)                      # metadataMap: {arraySize; {const char *key; metadata}[]}
    if smap:
        count = v.q(smap)
        if count > 1 << 24: raise Bad('string-keyed map of %d entries' % count)
        for i in range(count):
            k, m = v.ptr(smap + 8 + 16 * i), v.ptr(smap + 16 + 16 * i)
            if k and m: out.append((v.cstr(k), m))
    flat = ex.get('__swift_prespecializedPointerKeyedMetadataMap')
    if flat is not None:                        # {count; {key; metadata}[]}, keys named by their symbols
        count = v.q(flat)
        if count > 1 << 24: raise Bad('pointer-keyed map of %d entries' % count)
        for i in range(count):
            k, m = v.ptr(flat + 8 + 16 * i), v.ptr(flat + 16 + 16 * i)
            if m: out.append((names.get(k), m))
    elif minor >= 4 and v.ptr(data + 24):
        raise Bad('catalog %d.%d has a pointer-keyed map but no _swift_prespecializedPointerKeyedMetadataMap' % (major, minor))
    return out


def kind_of(v, m):
    k = v.q(m)
    return k if k < 0x800 else 'class'


def generic_optional_witnesses(v):
    """{slot: function} of the value witnesses in Optional's instantiation pattern ($sSqMn)"""
    ex = v.images.exports(SWIFTCORE)
    d = ex.get('_$sSqMn')
    if d is None: raise Bad('%s exports no $sSqMn' % SWIFTCORE)
    # TargetEnumDescriptor: flags, parent, name, access function, fields, payload cases, empty cases (7 words);
    # then TargetTypeGenericContextDescriptorHeader: instantiation cache, default instantiation pattern.
    if not v.u32(d) & 0x80: raise Bad('$sSqMn is not generic')
    pattern = v.rel(d + 32)
    vwt = v.rel(pattern + 12)                   # TargetGenericValueMetadataPattern: after function, function, flags
    w = {s: v.ptr(vwt + 8 * s) for s in OPTIONAL_SLOTS}
    lo, hi = v.images.segment(SWIFTCORE, '__TEXT')
    if not all(lo <= f < hi for f in w.values()): raise Bad('Optional pattern witnesses outside libswiftCore')
    return w


def gate_optional(v, entries):
    """(passed, report lines, {name: {slot: (got, owner)}})"""
    sqmn = v.images.exports(SWIFTCORE)['_$sSqMn']
    want = generic_optional_witnesses(v)
    bad, seen = {}, 0
    for name, m in entries:
        if kind_of(v, m) != KIND_OPTIONAL or v.ptr(m + 8) != sqmn: continue
        seen += 1
        vwt = v.ptr(m - 8)
        diff = {}
        for s in OPTIONAL_SLOTS:
            f = v.ptr(vwt + 8 * s)
            if f != want[s]: diff[s] = (f, v.images.owner(f))
        if diff: bad[name or '%#x' % m] = diff
    lines = ['gate 1 Optional witnesses: %d prespecialized Optionals, %d with other witnesses' % (seen, len(bad))]
    for n in sorted(bad):
        lines.append('  FAIL %s: %s' % (n, ', '.join('slot %d %#x (%s)' % (s, f, o or '?') for s, (f, o) in sorted(bad[n].items()))))
    return not bad, lines, bad


def gate_fast_paths(v, entries):
    """(report lines, counts): value types that cannot match swiftvalue's libswiftCore pointer checks"""
    ex = v.images.exports(SWIFTCORE)
    tables = {a: n for n, a in ex.items() if n.endswith('WV')}
    lo, hi = v.images.segment(SWIFTCORE, '__TEXT')
    stat, owners = collections.Counter(), collections.Counter()
    for name, m in entries:
        k = kind_of(v, m)
        if k == 'class':
            stat['class (no value witnesses compared)'] += 1
            continue
        kn = KIND_NAMES.get(k, '%#x' % k)
        vwt = v.ptr(m - 8)
        if vwt in tables:
            stat['%s: libswiftCore table %s' % (kn, tables[vwt].lstrip('_'))] += 1
            continue
        slots = range(8) if k not in (KIND_ENUM, KIND_OPTIONAL) else OPTIONAL_SLOTS
        out = [f for f in (v.ptr(vwt + 8 * s) for s in slots) if not lo <= f < hi]
        if out:
            stat['%s: MISS (witnesses outside libswiftCore)' % kn] += 1
            for f in set(out): owners[v.images.owner(f) or '?'] += 1
        else:
            stat['%s: own table, witnesses in libswiftCore' % kn] += 1
    miss = sum(n for k, n in stat.items() if 'MISS' in k)
    lines = ['gate 2 fast paths: %d entries, %d value types miss the libswiftCore pointer checks (slow path, not a failure)' %
             (len(entries), miss)]
    lines += ['  %6d  %s' % (n, k) for k, n in sorted(stat.items())]
    if owners:
        lines.append('  witnesses of the misses live in: ' + ', '.join('%s (%d)' % (os.path.basename(o), n) for o, n in owners.most_common(5)))
    return lines, dict(stat, miss=miss)


def text_digest(v, path):
    lo, hi = v.images.segment(path, '__TEXT')
    return hashlib.sha256(bytes(v.read(lo, hi - lo))).hexdigest()


def gate_bound_inputs(v, lock, contracts_dir, units=None, base=None):
    """(passed, report lines)"""
    ok, lines = True, ['gate 3 inputs bound to the cache (%s):' % v.uuid]
    with open(lock) as f:
        files = json.load(f)['files']
    cache_tables = [e for e in files if e.get('input_kind') == 'cache']
    stale = [e['path'] for e in cache_tables if e.get('cache_uuid', '').upper() != v.uuid]
    lines.append('  generated.lock: %d cache-derived tables, %d made for another cache' % (len(cache_tables), len(stale)))
    for p in stale:
        ok = False
        lines.append('  FAIL %s: made for cache %s, regenerate for %s' %
                     (p, next(e['cache_uuid'] for e in cache_tables if e['path'] == p), v.uuid))
    changed = set()
    if base is not None:
        lines.append('  base cache %s' % base.uuid)
    by_uuid = None

    def image_uuid_index():
        nonlocal by_uuid
        if by_uuid is None:
            by_uuid = {}
            for p in v.images.all_paths():
                u = v.images.uuid(p)
                if u: by_uuid.setdefault(u.lower(), p)
        return by_uuid

    def same_code(path):
        """False when BASE has this image with the same UUID but other __TEXT bytes"""
        if base is None or not base.images.has(path): return True
        if text_digest(v, path) == text_digest(base, path): return True
        changed.add(path)
        return False

    for cpath in sorted(glob.glob(os.path.join(contracts_dir, '*.json'))):
        with open(cpath) as f:
            c = json.load(f)
        t = c.get('targets', {}).get('x86_64')
        if not t: continue
        img = image_uuid_index().get(t['image_uuid'].lower())
        name = os.path.basename(cpath)
        if img is None:
            lines.append('  contract %s: its x86_64 image %s is not in this cache (not applicable)' % (name, t['image_uuid']))
            continue
        bad = []
        for a in t['anchors']:
            got = hashlib.sha256(bytes(v.read(a['address'], a['size']))).hexdigest()
            if got != a['sha256']: bad.append(a['role'])
        uuid_ok = t['cache_uuid'].upper() == v.uuid
        code_ok = same_code(img)
        if bad or not uuid_ok or not code_ok:
            ok = False
            why = []
            if not uuid_ok: why.append('proven for cache %s' % t['cache_uuid'].upper())
            if bad: why.append('anchor bytes changed: ' + ', '.join(bad))
            else: why.append('anchor hashes still match')
            if not code_ok: why.append('%s changed under the same UUID' % img)
            lines.append('  FAIL contract %s (%s): %s; re-prove it for %s' % (name, img, '; '.join(why), v.uuid))
        else:
            lines.append('  contract %s (%s): %d anchors re-proved' % (name, img, len(t['anchors'])))
    if units is not None:
        import hao_ifunit
        try:
            _, found, missing = hao_ifunit.match(units, v.path)
        except hao_ifunit.Bad as e:
            raise Bad(str(e))
        stale_units = sorted(n for n, p in found.items() if not same_code(p))
        lines.append('  interface units: %d match, %d missing, %d bound to changed code' % (len(found), len(missing), len(stale_units)))
        for n, why in missing:
            ok = False
            lines.append('  FAIL unit %s: %s' % (n, why))
        for n in stale_units:
            ok = False
            lines.append('  FAIL unit %s: %s changed under the same UUID' % (n, found[n]))
    if base is not None:
        lines.append('  images changed under the same UUID among those checked: %d%s' %
                     (len(changed), (' (' + ', '.join(sorted(changed)) + ')') if changed else ''))
    return ok, lines


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cache')
    ap.add_argument('--base-cache', help='the cache the units, contracts and tables were made for')
    ap.add_argument('--units', help='interface unit directory (payload units-input)')
    ap.add_argument('--lock', default=os.path.join(HERE, '..', 'generated.lock'))
    ap.add_argument('--contracts', default=os.path.join(HERE, 'value-contracts'))
    ap.add_argument('--json', help='write the result here')
    a = ap.parse_args(argv)
    try:
        v = open_cache(a.cache)
        base = open_cache(a.base_cache) if a.base_cache else None
        entries = catalog_entries(v)
        print('cache %s: catalog %s, %d prespecialized metadata' %
              (v.uuid, 'present' if v.images.has(PRESPECIALIZED) else 'absent', len(entries)))
        ok1, l1, bad1 = gate_optional(v, entries)
        l2, stat2 = gate_fast_paths(v, entries)
        ok3, l3 = gate_bound_inputs(v, a.lock, a.contracts, a.units, base)
    except Bad as e:
        print('hao_catalog_gate: %s' % e, file=sys.stderr)
        return 2
    print('\n'.join(l1 + l2 + l3))
    ok = ok1 and ok3
    print('PASS' if ok else 'FAIL')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'cache_uuid': v.uuid, 'entries': len(entries), 'pass': ok,
                       'optional': {'pass': ok1, 'other_witnesses': sorted(bad1)},
                       'fast_paths': stat2, 'bound_inputs': {'pass': ok3, 'lines': l3}}, f, indent=1)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
