# Vendored from hao-assets (hao_contract.py at commit df3658e); update by copying the file from a newer hao-assets
# commit and changing the pin in tools/hao-contract/PIN. Do not edit here.
#!/usr/bin/env python3
"""hao-contract: the interfaces between the research, runtime and patcher repositories, checked (workflow plan step 3).

  hao_contract.py check REPO            run the checks REPO/contract.json names; one line per problem, exit 1
  hao_contract.py lock REPO GLOB... --os-build B [--input-kind K] [--sdk-build S] [--cache-uuid U] [--format F]
                                        print a generated.lock for the files GLOB matches (review, then commit)
  hao_contract.py stamps REPO GLOB...   the OS build stamped in each generated file's header

REPO/contract.json: {"contract_version": 1, "role": "research" | "runtime" | "patcher", "checks": [...]}, where a
check is one of
  {"kind": "hygiene", "allow": {PATH: [PATTERN LABELS]}, "commits": N}
        no tracked file (and none of the last N commit messages) names a private location
  {"kind": "generated-lock", "lock": "generated.lock", "globs": ["src/*_gen.inc"], "stamp_header": PATH}
        every file the globs match is listed with its sha256, export id, format and input_kind; every listed file
        matches; a cache-kind file must be stamped with its os_build (the 26B131-rows-under-a-26A428-stamp case);
        an sdk+census file stamped for another build is a coverage note; an sdk-kind file needs only sdk_build;
        with stamp_header, every cache table's os_build equals its HAO_GEN_OS_BUILD and every format its
        HAO_GEN_FORMAT (the runtime's one startup stamp)
  {"kind": "payload", "manifest": PATH, "os_build": B, "layout_version": N}
        a hao-runtime payload-manifest.json: clean, the declared OS build and install-layout version, and every
        listed file present with its sha256
Standard library only, so each repo can vendor a pinned copy.
"""
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

CONTRACT_VERSION = 1
# Built from parts so this file does not match itself.
PRIVATE = [
    ('private research repository', re.compile(re.escape('hao-' 'research'))),
    ('private evidence folder', re.compile(re.escape('store/' 'evidence'))),
    ('archived private tooling', re.compile(re.escape('ssd-tools-' 'archive'))),
    ('home folder', re.compile('/' r'Users/(?!Shared\b)[A-Za-z]')),
]
STAMP = re.compile(r'\b(\d{2}[A-Z]\d{2,4}[a-z]?)\)')
LOCK_FIELDS = ('path', 'sha256', 'export_id', 'format', 'input_kind')
# What each input kind must name, and what a header stamp is compared with:
#   sdk         SDK headers only: sdk_build; the stamp is not tied to an OS build
#   sdk+census  SDK headers + an OS build's import census: sdk_build and os_build (the census's); a stamp
#               for another OS build is a coverage note, not a failure
#   cache       shared caches: os_build and cache_uuid; the stamp must equal os_build
INPUT_KINDS = {'sdk': ('sdk_build',), 'sdk+census': ('sdk_build', 'os_build'), 'cache': ('os_build', 'cache_uuid')}
PAYLOAD_FIELDS = ('version', 'commit', 'clean', 'os_build', 'cache_uuid', 'units_version', 'units_sha256',
                  'layout_version', 'files')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tracked(repo):
    out = subprocess.run(['git', '-C', str(repo), 'ls-files', '-z'], capture_output=True, check=True).stdout
    return [n for n in out.decode().split('\0') if n]


def stamp_of(path):
    with open(path, 'rb') as f:
        head = f.read(512).decode('utf8', 'replace').splitlines()[:3]
    for line in head:
        m = STAMP.search(line)
        if m:
            return m.group(1)
    return None


def check_hygiene(repo, c):
    bad = []
    allow = c.get('allow', {})
    for name in tracked(repo):
        path = Path(repo) / name
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        if b'\0' in data[:8192]:
            continue
        text = data.decode('utf8', 'replace')
        for label, pattern in PRIVATE:
            if label in allow.get(name, []):
                continue
            for n, line in enumerate(text.splitlines(), 1):
                if pattern.search(line):
                    bad.append('hygiene: %s:%d names a %s' % (name, n, label))
                    break
    n = c.get('commits', 0)
    if n:
        log = subprocess.run(['git', '-C', str(repo), 'log', '-%d' % n, '--format=%h%x00%B%x1e'],
                             capture_output=True, text=True, check=True).stdout
        for entry in filter(str.strip, log.split('\x1e')):
            h, _, body = entry.strip().partition('\0')
            for label, pattern in PRIVATE:
                if pattern.search(body):
                    bad.append('hygiene: commit %s message names a %s' % (h, label))
    return bad


def check_lock(repo, c):
    repo = Path(repo)
    lock_path = repo / c.get('lock', 'generated.lock')
    if not lock_path.is_file():
        return ['generated-lock: %s is missing' % lock_path.name]
    try:
        lock = json.loads(lock_path.read_text())
    except ValueError as x:
        return ['generated-lock: %s is not JSON (%s)' % (lock_path.name, x)]
    bad, listed = [], {}
    notes = []
    for e in lock.get('files', []):
        kind = e.get('input_kind')
        if kind not in INPUT_KINDS:
            bad.append('generated-lock: %s: input_kind must be one of %s' % (e.get('path', '?'), ', '.join(INPUT_KINDS)))
            continue
        missing = [f for f in LOCK_FIELDS + INPUT_KINDS[kind] if f not in e]
        if missing:
            bad.append('generated-lock: %s lacks %s' % (e.get('path', '?'), ', '.join(missing)))
            continue
        listed[e['path']] = e
        p = repo / e['path']
        if not p.is_file():
            bad.append('generated-lock: %s is listed but missing' % e['path'])
            continue
        if sha256(p) != e['sha256']:
            bad.append('generated-lock: %s differs from its locked sha256 (edited by hand, or not re-exported)'
                       % e['path'])
        stamp = stamp_of(p)
        if stamp and kind == 'cache' and stamp != e['os_build']:
            bad.append('generated-lock: %s is stamped %s but locked as %s' % (e['path'], stamp, e['os_build']))
        elif stamp and kind == 'sdk+census' and stamp != e['os_build']:
            notes.append('generated-lock: note: %s covers %s\'s imports (stamp), locked census %s: coverage, not '
                         'correctness' % (e['path'], stamp, e['os_build']))
    header = c.get('stamp_header')
    if header:
        text = (repo / header).read_text() if (repo / header).is_file() else ''
        build = re.search(r'#define\s+HAO_GEN_OS_BUILD\s+"([^"]+)"', text)
        fmt = re.search(r'#define\s+HAO_GEN_FORMAT\s+(\d+)', text)
        if not build or not fmt:
            bad.append('generated-lock: %s does not define HAO_GEN_OS_BUILD and HAO_GEN_FORMAT' % header)
        else:
            for e in listed.values():
                if e['input_kind'] == 'cache' and e['os_build'] != build.group(1):
                    bad.append('generated-lock: %s is a cache table for %s, but %s stamps %s' % (
                        e['path'], e['os_build'], header, build.group(1)))
                if str(e['format']) != fmt.group(1):
                    bad.append('generated-lock: %s has format %s, %s says %s' % (e['path'], e['format'], header,
                                                                               fmt.group(1)))
    for name in tracked(repo):
        if any(fnmatch.fnmatch(name, g) for g in c.get('globs', [])) and name not in listed:
            bad.append('generated-lock: %s is generated but not in %s' % (name, lock_path.name))
    for n in notes:
        print(n, file=sys.stderr)
    return bad


def check_payload(repo, c):
    path = Path(repo) / c['manifest'] if not os.path.isabs(c['manifest']) else Path(c['manifest'])
    if not path.is_file():
        return ['payload: %s is missing' % path]
    m = json.loads(path.read_text())
    bad = ['payload: manifest lacks %s' % f for f in PAYLOAD_FIELDS if f not in m]
    if bad:
        return bad
    if m['clean'] is not True:
        bad.append('payload: built from a dirty tree')
    if m['os_build'] != c['os_build']:
        bad.append('payload: OS build %s, the patcher declares %s' % (m['os_build'], c['os_build']))
    if m['layout_version'] != c['layout_version']:
        bad.append('payload: install-layout version %s, the patcher declares %s' % (m['layout_version'],
                                                                                   c['layout_version']))
    root = path.parent
    for rel, digest in sorted(m['files'].items()):
        f = root / rel
        if not f.is_file():
            bad.append('payload: %s is listed but missing' % rel)
        elif sha256(f) != digest:
            bad.append('payload: %s does not match its sha256' % rel)
    return bad


CHECKS = {'hygiene': check_hygiene, 'generated-lock': check_lock, 'payload': check_payload}


def check(repo):
    contract = Path(repo) / 'contract.json'
    if not contract.is_file():
        return ['%s has no contract.json' % repo]
    c = json.loads(contract.read_text())
    if c.get('contract_version') != CONTRACT_VERSION:
        return ['contract.json: contract_version %s, this checker is %d' % (c.get('contract_version'), CONTRACT_VERSION)]
    bad = []
    for item in c.get('checks', []):
        fn = CHECKS.get(item.get('kind'))
        bad += fn(repo, item) if fn else ['contract.json: unknown check %r' % item.get('kind')]
    return bad


def main(argv):
    if len(argv) >= 2 and argv[0] == 'check':
        bad = check(argv[1])
        print('\n'.join(bad) or 'hao-contract: %s keeps its contract' % argv[1])
        return 1 if bad else 0
    if len(argv) >= 3 and argv[0] == 'stamps':
        repo = Path(argv[1])
        for name in sorted(n for n in tracked(repo) if any(fnmatch.fnmatch(n, g) for g in argv[2:])):
            print('%-12s %s' % (stamp_of(repo / name) or '-', name))
        return 0
    if len(argv) >= 3 and argv[0] == 'lock' and '--os-build' in argv:
        opts = {argv[i]: argv[i + 1] for i in range(len(argv) - 1) if argv[i].startswith('--')}
        repo = Path(argv[1])
        globs = [a for a in argv[2:] if not a.startswith('--') and a not in opts.values()]
        files = []
        for name in sorted(n for n in tracked(repo) if any(fnmatch.fnmatch(n, g) for g in globs)):
            files.append({'path': name, 'sha256': sha256(repo / name), 'export_id': None, 'format': opts.get('--format'),
                      'input_kind': opts.get('--input-kind', 'cache'), 'os_build': opts['--os-build'],
                      'sdk_build': opts.get('--sdk-build'), 'cache_uuid': opts.get('--cache-uuid')})
        print(json.dumps({'lock_version': 1, 'files': files}, indent=1))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
