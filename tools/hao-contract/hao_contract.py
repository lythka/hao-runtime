# Vendored from hao-patcher (tools/hao-contract/hao_contract.py at commit c1ee8207); update by copying the file from a
# newer hao-patcher commit and changing the pin in tools/hao-contract/PIN. Do not edit here.
#!/usr/bin/env python3
"""hao-contract: the interfaces between the hao repositories, checked. Its home is hao-patcher
(tools/hao-contract); other repositories vendor a pinned copy.

  hao_contract.py check REPO            run the checks REPO/contract.json names; one line per problem, exit 1
  hao_contract.py message FILE          the hygiene check on one commit message (a commit-msg hook); exit 1
  hao_contract.py release FILE [DIR]    check a release.json, and with DIR every asset file staged there; exit 1
  hao_contract.py lock REPO GLOB... --os-build B [--input-kind K] [--sdk-build S] [--cache-uuid U] [--format F]
                                        print a generated.lock for the files GLOB matches (review, then commit)
  hao_contract.py stamps REPO GLOB...   the OS build stamped in each generated file's header
  hao_contract.py --version             the checker's version

REPO/contract.json: {"contract_version": 1, "role": "research" | "runtime" | "patcher", "checks": [...]}, where a
check is one of
  {"kind": "hygiene", "allow": {PATH: [PATTERN LABELS]}, "commits": N}
        no tracked file (and none of the last N commit messages) names a home folder or a private name. Private
        names are not kept in this file: they are read from $HAO_CONTRACT_PRIVATE or
        ~/.config/hao-contract/private-names, one "LABEL: TEXT" per line (# starts a comment). Without that
        file only home folders are checked
  {"kind": "generated-lock", "lock": "generated.lock", "globs": ["src/*_gen.inc"], "stamp_header": PATH}
        every file the globs match is listed with its sha256, export id, format and input_kind; every listed file
        matches; a cache-kind file must be stamped with its os_build;
        an sdk+census file stamped for another build is a coverage note; an sdk-kind file needs only sdk_build;
        with stamp_header, every cache table's os_build equals its HAO_GEN_OS_BUILD and every format its
        HAO_GEN_FORMAT (the runtime's one startup stamp)
  {"kind": "payload", "manifest": PATH, "os_build": B, "layout_version": N}
        a hao-runtime payload-manifest.json: clean, the declared OS build and install-layout version, and every
        listed file present with its sha256
  {"kind": "release", "release": PATH, "dir": PATH, "url_prefix": URL}
        a release.json (schemas/release.schema.json): every download and asset has url, size and sha256; asset urls
        are URL_PREFIX/<release>/<file> (default the hao-assets releases); no row is both an asset and made on the
        user's Mac, and no harvested row ships, except as an asset marked "harvested": true for a target in
        HARVESTED_ASSET_TARGETS (mbp92); every made-on-Mac row names its producer. With dir, every asset
        file is there with its size and sha256, and nothing else is
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

__version__ = '0.2.0'
CONTRACT_VERSION = 1
HOME = ('home folder', re.compile('/' r'Users/(?!Shared\b)[A-Za-z]'))   # built from parts: no self-match


def private_names(path=None):
    """[(label, pattern)]: home folders, plus the local private-names file's entries."""
    path = Path(path or os.environ.get('HAO_CONTRACT_PRIVATE') or
                Path.home() / '.config' / 'hao-contract' / 'private-names')
    names = [HOME]
    if path.is_file():
        for line in path.read_text().splitlines():
            line = line.split('#', 1)[0].strip()
            label, sep, text = line.partition(':')
            if sep and text.strip():
                names.append((label.strip(), re.compile(re.escape(text.strip()))))
    return names


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
    names = private_names()
    for name in tracked(repo):
        path = Path(repo) / name
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        if b'\0' in data[:8192]:
            continue
        text = data.decode('utf8', 'replace')
        for label, pattern in names:
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
            for label, pattern in names:
                if pattern.search(body):
                    bad.append('hygiene: commit %s message names a %s' % (h, label))
    return bad


def check_message(text):
    """Hygiene on one commit message; comment lines (git's template) are ignored."""
    body = '\n'.join(l for l in text.splitlines() if not l.startswith('#'))
    return ['hygiene: commit message names a %s' % label for label, pattern in private_names()
            if pattern.search(body)]


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


RELEASE_FIELDS = ('contract_version', 'release', 'prerelease', 'proven_models', 'assembly', 'golden_build',
                  'donor_build', 'oclp', 'hao_patcher', 'hao_runtime', 'downloads', 'assets', 'made_on_mac',
                  'harvested', 'expected', 'version', 'channel')
DOWNLOAD_FIELDS = ('row', 'name', 'url', 'size', 'sha256')
ASSET_FIELDS = ('row', 'file', 'url', 'size', 'sha256', 'commit', 'format')
ASSET_FORMATS = ('file', 'tar')
# Targets whose release may publish a harvested row (caches made on a running laptop of that model) as an asset marked
# "harvested": true. Owner, 2026-11-14: "Ship the caches for 9,2." Every other target still refuses one; release-cut
# publishes them only while pipeline/release-classes.json's harvested_assets turns it on for the target.
HARVESTED_ASSET_TARGETS = ('mbp92',)
URL_PREFIX = 'https://github.com/lythka/hao-assets/releases/download'
# Other public release hosts an asset may name (published there, fetched from there; not staged for hao-assets).
OTHER_HOSTS = ('https://github.com/lythka/hao-runtime/releases/download/',)
RELEASE_NAME = re.compile(r'^\d+(\.\d+){1,2}-\d{2}[A-Z]\d{2,4}[a-z]?\.\d+$')
# release.json's own format version (2: version and channel, 2026-11-14); contract.json keeps CONTRACT_VERSION
RELEASE_FORMAT = 2
RELEASE_VERSION = re.compile(r'^v\d+\.\d+\.\d+(-(alpha|beta)\.\d+)?$')   # v0.1.0, v0.1.0-alpha.1
# Channels, earliest first. Owner, 2026-11-13 ("How do we know which is beta? Is that in the .json?"). Each is a
# prerelease until a later, final channel exists; beta is cut complete (no pending rows), dev and alpha may not be.
CHANNELS = ('dev', 'alpha', 'beta')
COMPLETE_CHANNELS = ('beta',)
HEX64 = re.compile(r'^[0-9a-f]{64}$')


def check_release_file(path, stage=None, url_prefix=URL_PREFIX):
    """Problems with a release.json and, given stage (a folder), with the asset files staged in it."""
    path = Path(path)
    if not path.is_file():
        return ['release: %s is missing' % path]
    try:
        r = json.loads(path.read_text())
    except ValueError as x:
        return ['release: %s is not JSON (%s)' % (path.name, x)]
    bad = ['release: lacks %s' % f for f in RELEASE_FIELDS if f not in r]
    if bad:
        return bad
    if r['contract_version'] != RELEASE_FORMAT:
        bad.append('release: contract_version %r, this checker is %d' % (r['contract_version'], RELEASE_FORMAT))
    if not isinstance(r['version'], str) or not RELEASE_VERSION.match(r['version']):
        bad.append('release: version %r is not vX.Y.Z or vX.Y.Z-alpha.N / -beta.N (e.g. v0.1.0-alpha.1)' % r['version'])
    if r['channel'] not in CHANNELS:
        bad.append('release: channel %r is not one of %s' % (r['channel'], ', '.join(CHANNELS)))
    elif r['channel'] in COMPLETE_CHANNELS and r.get('state', 'complete') != 'complete':
        bad.append('release: a %s release must be complete (state %r)' % (r['channel'], r.get('state')))
    if r['channel'] in CHANNELS and r['prerelease'] is not True:
        bad.append('release: a %s release is a prerelease (there is no final channel yet)' % r['channel'])
    name = r['release']
    if not isinstance(name, str) or not RELEASE_NAME.match(name):
        bad.append('release: name %r is not MACOS-BUILD.N (e.g. 27.2.1-26B131.0)' % name)
    if not isinstance(r['prerelease'], bool):
        bad.append('release: prerelease must be true or false')

    def pinned(kind, e, fields):
        label = '%s %s' % (kind, e.get('row', '?'))
        out = ['release: %s lacks %s' % (label, f) for f in fields if f not in e or e[f] in (None, '')]
        if out:
            return out
        if not isinstance(e['size'], int) or isinstance(e['size'], bool) or e['size'] < 0:
            out.append('release: %s size %r is not a byte count' % (label, e['size']))
        if not HEX64.match(str(e['sha256'])):
            out.append('release: %s sha256 is not 64 lowercase hex digits' % label)
        if not str(e['url']).startswith('https://'):
            out.append('release: %s url is not https' % label)
        return out

    rows = {}
    for e in r['downloads']:
        bad += pinned('download', e, DOWNLOAD_FIELDS)
        rows.setdefault(e.get('row'), []).append('downloads')
    files, hosted = set(), set()
    for e in r['assets']:
        # An asset with no public producing commit is published by its sha256 alone: no repo and commit "".
        sha_only = 'repo' not in e and e.get('commit') == ''
        b = pinned('asset', e, tuple(f for f in ASSET_FIELDS if not (sha_only and f == 'commit')))
        bad += b
        rows.setdefault(e.get('row'), []).append('assets')
        if b:
            continue
        if e.get('harvested') is not None and not isinstance(e.get('harvested'), bool):
            bad.append('release: asset %s harvested must be true or false' % e['row'])
        elif e.get('harvested') and r.get('target') not in HARVESTED_ASSET_TARGETS:
            bad.append('release: asset %s is harvested, and target %s may not ship harvested rows (only %s)' % (
                e['row'], r.get('target'), ', '.join(HARVESTED_ASSET_TARGETS)))
        if e['format'] not in ASSET_FORMATS:
            bad.append('release: asset %s format %r is not one of %s' % (e['row'], e['format'], ', '.join(ASSET_FORMATS)))
        if '/' in e['file'] or e['file'] in ('', '.', '..'):
            bad.append('release: asset %s file %r is not a plain file name' % (e['row'], e['file']))
        if e['file'] in files:
            bad.append('release: asset file %s is listed twice' % e['file'])
        files.add(e['file'])
        want = '%s/%s/%s' % (url_prefix.rstrip('/'), name, e['file'])
        if any(e['url'].startswith(h) for h in OTHER_HOSTS):
            if not e['url'].endswith('/' + e['file']):
                bad.append('release: asset %s url %s does not end in its file' % (e['row'], e['url']))
            hosted.add(e['file'])
        elif e['url'] != want:
            bad.append('release: asset %s url %s is not %s' % (e['row'], e['url'], want))
    for e in r['made_on_mac']:
        rows.setdefault(e.get('row'), []).append('made_on_mac')
        if not e.get('producer'):
            bad.append('release: made_on_mac %s names no producer' % e.get('row', '?'))
        if e.get('url'):
            bad.append('release: made_on_mac %s has a url: it is made on the user\'s Mac, never uploaded' % e.get('row'))
        if not isinstance(e.get('inputs', []), list):
            bad.append('release: made_on_mac %s inputs is not a list' % e.get('row'))
    for row in r['harvested']:
        rows.setdefault(row, []).append('harvested')
    for row, where in sorted(rows.items(), key=lambda x: str(x[0])):
        if row is None:
            bad.append('release: an entry in %s has no row' % where[0])
        elif len(where) > 1:
            bad.append('release: row %s is in %s' % (row, ' and '.join(where)))
    if not isinstance(r['expected'], dict) or not all(HEX64.match(str(v)) for v in r['expected'].values()):
        bad.append('release: expected must map each step to a sha256')
    if stage is not None:
        stage = Path(stage)
        for e in r['assets']:
            f = (stage / 'external' if e.get('file') in hosted else stage) / str(e.get('file', ''))
            if not e.get('file') or not f.is_file():
                bad.append('release: asset %s is not staged in %s' % (e.get('file'), stage))
            elif f.stat().st_size != e.get('size'):
                bad.append('release: staged %s is %d bytes, release.json says %s' % (e['file'], f.stat().st_size,
                                                                                    e.get('size')))
            elif sha256(f) != e.get('sha256'):
                bad.append('release: staged %s does not match its sha256' % e['file'])
        extra = sorted(p.name for p in stage.iterdir() if p.is_file() and p.name not in files - hosted and p != path
                       and p.name != path.name and not p.name.startswith('.'))
        bad += ['release: %s is staged but not an asset (it would not be uploaded)' % n for n in extra]
    return bad


def check_release(repo, c):
    base = Path(repo)
    resolve = lambda p: Path(p) if os.path.isabs(p) else base / p
    return check_release_file(resolve(c['release']), resolve(c['dir']) if c.get('dir') else None,
                              c.get('url_prefix', URL_PREFIX))


CHECKS = {'hygiene': check_hygiene, 'generated-lock': check_lock, 'payload': check_payload, 'release': check_release}


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
    if argv[:1] == ['--version']:
        print('hao-contract %s (contract_version %d)' % (__version__, CONTRACT_VERSION))
        return 0
    if len(argv) == 2 and argv[0] == 'message':
        bad = check_message(Path(argv[1]).read_text(errors='replace'))
        for b in bad:
            print(b, file=sys.stderr)
        return 1 if bad else 0
    if argv[:1] == ['release'] and len(argv) in (2, 3):
        bad = check_release_file(argv[1], argv[2] if len(argv) == 3 else None)
        print('\n'.join(bad) or 'hao-contract: %s is a well-formed release' % argv[1])
        return 1 if bad else 0
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
