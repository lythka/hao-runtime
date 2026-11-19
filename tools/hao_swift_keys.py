#!/usr/bin/env python3
"""hao_swift_keys.py LOG... [--walk-table INC] [--cache CACHE | --catalog-names FILE] [--tsv OUT] [--specs OUT] [--json OUT]

Coverage of hao's Swift tables by the keys programs met. LOG files hold the
records hao writes where its diagnostics go (stderr, hao_crash's
console.log, `log show` output): "hao-swift-key {JSON}" (HAO_SWIFT_KEYS or
boot argument hao_swift_keys=1: each Swift type the value walk looked up in
its facts table, once per process, with the table's answer and where the
metadata lies) and "hao-swift-audit {JSON}" (each type of a value hao
could not certify, once per type and reason). Keys are the walk table's:
mangled names without the "$s" prefix, private contexts' addresses removed.

Reported:
  walk table   keys met by the table's answer; with --walk-table (the
               generated src/swiftwalk_gen.inc) also the keys recorded as
               "other-cache" (the table was for another x86_64 cache) that
               the table lists;
  metadata     keys by where their metadata lay: "cache" (laid down in a
               cache image: static or prespecialized), "heap" (instantiated
               at run time), "guest" (a guest image's type); with --cache
               (an x86_64 shared cache, read as tools/hao_catalog_gate.py
               does) or --catalog-names (one name a line), the heap keys
               the catalog names and the native ones it does not.
--tsv writes one row per key: key, rows, where, processes, uncertified
reasons, x86_64 cache UUIDs. --specs writes the keys the walk met without
a row, and those of uncertified values, as walk-facts generator input
("mangled:KEY", guest-defined types as comments). Exit 0, or 2 when a
file cannot be read."""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECORD = re.compile(r'hao-swift-(key|audit) (\{.*\})\s*$')
ROWED = ('same', 'dynamic', 'convert', 'refuse', 'guest', 'guest-incomplete', 'guest-optional')
UNROWED = ('none', 'layout', 'identity')


def read_records(paths):
    """{key: entry}: rows, where, processes, reasons, caches, kinds; and the count of records read"""
    keys = collections.defaultdict(lambda: {'rows': set(), 'where': set(), 'processes': set(), 'reasons': set(),
                                            'caches': set(), 'kind': None})
    n = 0
    for p in paths:
        with open(p, 'rb') as f:
            for raw in f:
                m = RECORD.search(raw.decode('utf-8', 'replace'))
                if not m: continue
                try: r = json.loads(m.group(2))
                except ValueError: continue
                if not isinstance(r, dict) or r.get('v') != 1: continue
                key = r.get('key' if m.group(1) == 'key' else 'type')
                if not isinstance(key, str) or not key: continue
                n += 1
                e = keys[key]
                if r.get('process'): e['processes'].add(str(r['process']))
                if r.get('x86_cache'): e['caches'].add(str(r['x86_cache']))
                if e['kind'] is None and isinstance(r.get('kind'), int): e['kind'] = r['kind']
                if m.group(1) == 'key':
                    if r.get('row'): e['rows'].add(str(r['row']))
                    if r.get('where'): e['where'].add(str(r['where']))
                elif r.get('reason'):
                    e['reasons'].add(str(r['reason']))
    return dict(keys), n


def walk_table_keys(path):
    """the keys of src/swiftwalk_gen.inc's SWIFTWALK_TYPES rows"""
    out, on = set(), False
    with open(path, encoding='utf-8') as f: lines = f.readlines()
    for line in lines:
        s = line.strip()
        if s.startswith('#if defined(SWIFTWALK_TYPES)'): on = True
        elif s.startswith('#if defined(') or s.startswith('#endif'): on = False
        elif on and s.startswith('{"'):
            out.add(json.loads(s[1:s.index('",') + 1]))
    return out


def catalog_names(cache):
    sys.path.insert(0, HERE)
    import hao_catalog_gate as g
    v = g.open_cache(cache)
    return {n for n, _ in g.catalog_entries(v) if n}, v.uuid


def answer(e):
    """one answer per key: a row if any process found one, else the most telling miss"""
    for r in ROWED + UNROWED + ('other-cache',):
        if r in e['rows']: return r
    return 'unrecorded'   # only audit records name it


def report(keys, table=None, catalog=None):
    out = {'keys': len(keys), 'answers': collections.Counter(), 'where': collections.Counter()}
    for e in keys.values():
        out['answers'][answer(e)] += 1
        for w in e['where'] or ('unknown',): out['where'][w] += 1
    rowed = sum(out['answers'][r] for r in ROWED)
    looked = rowed + sum(out['answers'][r] for r in UNROWED)
    out['walk_coverage'] = [rowed, looked]
    out['uncertified'] = sorted(k for k, e in keys.items() if e['reasons'])
    if table is not None:
        out['other_cache_listed'] = sorted(k for k, e in keys.items() if answer(e) == 'other-cache' and k in table)
    heap = sorted(k for k, e in keys.items() if 'heap' in e['where'])
    out['heap'] = len(heap)
    if catalog is not None:
        out['catalog_names'] = len(catalog)
        out['heap_in_catalog'] = sorted(k for k in heap if k in catalog)
        out['heap_not_in_catalog'] = len(heap) - len(out['heap_in_catalog'])
    return out


def write_tsv(keys, path):
    with open(path, 'w') as f:
        f.write('key\trows\twhere\tprocesses\treasons\tx86_cache\n')
        for k in sorted(keys):
            e = keys[k]
            f.write('\t'.join([k] + [','.join(sorted(e[c])) for c in ('rows', 'where', 'processes', 'reasons', 'caches')]) + '\n')


def write_specs(keys, path):
    """walk-facts generator input: uncertified first, then met without a row"""
    def line(k, e):
        procs = sorted(e['processes'])
        note = '%s%s' % (', '.join(procs[:4]), ' +%d' % (len(procs) - 4) if len(procs) > 4 else '')
        if e['reasons']: note += '; ' + '; '.join(sorted(e['reasons'])[:2])
        return ('# guest-defined: ' if e['where'] == {'guest'} else '') + 'mangled:%s  # %s\n' % (k, note)
    unc = [k for k in sorted(keys) if keys[k]['reasons']]
    miss = [k for k in sorted(keys) if not keys[k]['reasons'] and answer(keys[k]) in UNROWED + ('other-cache',)]
    with open(path, 'w') as f:
        f.write('# Swift keys hao met (tools/hao_swift_keys.py): values it could not certify\n')
        f.writelines(line(k, keys[k]) for k in unc)
        f.write('# met by the value walk with no row\n')
        f.writelines(line(k, keys[k]) for k in miss)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--walk-table', help='src/swiftwalk_gen.inc')
    src = ap.add_mutually_exclusive_group()
    src.add_argument('--cache', help='x86_64 shared cache (directory or main file) whose catalog to compare with')
    src.add_argument('--catalog-names', help='catalog names, one a line')
    ap.add_argument('--tsv')
    ap.add_argument('--specs')
    ap.add_argument('--json')
    a = ap.parse_args(argv)
    try:
        keys, n = read_records(a.logs)
        table = walk_table_keys(a.walk_table) if a.walk_table else None
        catalog, cache_uuid = None, None
        if a.cache: catalog, cache_uuid = catalog_names(a.cache)
        elif a.catalog_names:
            with open(a.catalog_names) as f: catalog = {l.strip() for l in f if l.strip() and not l.startswith('#')}
    except Exception as e:   # unreadable input, or a cache hao_catalog_gate refuses
        print('hao_swift_keys: %s' % e, file=sys.stderr)
        return 2
    r = report(keys, table, catalog)
    print('%d records, %d keys' % (n, r['keys']))
    print('walk table: %d of %d keys looked up have a row; answers: %s' % (r['walk_coverage'][0], r['walk_coverage'][1],
          ', '.join('%s %d' % kv for kv in sorted(r['answers'].items()))))
    print('uncertified values: %d keys' % len(r['uncertified']))
    if table is not None:
        print('recorded under another cache but listed in the table: %d keys' % len(r['other_cache_listed']))
    print('metadata: %s' % ', '.join('%s %d' % kv for kv in sorted(r['where'].items())))
    if catalog is not None:
        print('catalog%s: %d names; heap keys it names %d, heap keys it does not %d' %
              (' ' + cache_uuid if cache_uuid else '', r['catalog_names'], len(r['heap_in_catalog']), r['heap_not_in_catalog']))
    if a.tsv: write_tsv(keys, a.tsv)
    if a.specs: write_specs(keys, a.specs)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(dict(r, answers=dict(r['answers']), where=dict(r['where']), cache_uuid=cache_uuid), f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
