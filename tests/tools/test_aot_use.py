"""tools/hao-aot-use: AOT use records (src/aot_use.c) from several runs and log styles -> an AOT list.
A ROOT with an arm64e program, a native one, a guest dylib and none for a shared-cache image; checks the order (runs,
then translated blocks), the cumulative jit counts (a run's largest record), --min-runs, --programs-only, --base,
what --root leaves out and why, the report, and that a log without records fails.
  python3 -I tests/tools/test_aot_use.py OUT"""
import json, os, shutil, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, '..', '..', 'tools', 'hao-aot-use')
out = sys.argv[1]
bad = 0


def check(ok, what):
    global bad
    print(('PASS ' if ok else 'FAIL ') + what)
    bad += not ok


def macho(cpu, uuid):
    """a thin 64-bit Mach-O executable with one LC_UUID"""
    return struct.pack('<IiiIIIII', 0xFEEDFACF, cpu, 2, 2, 1, 24, 0, 0) + struct.pack('<II', 0x1B, 24) + bytes.fromhex(uuid)


ARM, X86 = 0x0100000C, 0x01000007
UA, UB, UL, UN = '11' * 16, '22' * 16, '33' * 16, '44' * 16
shutil.rmtree(out, ignore_errors=True)
root = os.path.join(out, 'root')
for rel, data in (('usr/bin/a', macho(ARM, UA)), ('usr/libexec/b', macho(ARM, '55' * 16)), ('usr/lib/libl.dylib', macho(ARM, UL)),
                  ('usr/libexec/native', macho(X86, UN))):
    os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
    open(os.path.join(root, rel), 'wb').write(data)


def rec(run, kind, *f):
    return 'hao-use 1 %016x %s %s' % (run, kind, ' '.join(str(x) for x in f))


# a: 3 runs (one in a compact log line, one from -v stderr), b: 2 runs with more JIT blocks; libl in a's runs;
# the shared-cache image and the native program ran once each
compact = '2026-11-18 18:39:43.044 Df hao[91558:92aa924] [com.hao:use] '
logs = [compact + rec(1, 'main', UA, 'none', '/usr/bin/a'), compact + rec(1, 'image', UL, 'none', '/usr/lib/libl.dylib'),
        compact + rec(1, 'jit', 10, UA, '/usr/bin/a'), compact + rec(1, 'jit', 40, UA, '/usr/bin/a'),
        compact + rec(1, 'jit', 5, UL, '/usr/lib/libl.dylib'),
        rec(2, 'main', UA, 'aot', '/usr/bin/a'), rec(3, 'main', UA, 'rejected', '/usr/bin/a'),
        rec(4, 'main', UB, 'none', '/usr/libexec/b'), rec(4, 'jit', 900, UB, '/usr/libexec/b'),
        rec(5, 'main', UB, 'unfit', '/usr/libexec/b'),
        rec(6, 'image', '66' * 16, 'off', '/System/Library/PrivateFrameworks/X.framework/Versions/A/X'),
        rec(7, 'main', UN, 'none', '/usr/libexec/native'),
        'hao-use 1 0000000000000008 main zz none /usr/bin/bad', 'unrelated line']
os.makedirs(out, exist_ok=True)
lp = os.path.join(out, 'use.log')
open(lp, 'w').write('\n'.join(logs) + '\n')
base = os.path.join(out, 'base')
open(base, 'w').write('# fixed\n/usr/libexec/b\nusr/sbin/extra\n')


def tool_run(*args, stdin=None):
    p = subprocess.run([sys.executable, '-I', tool, *args], capture_output=True, text=True, input=stdin)
    return p.returncode, [l for l in p.stdout.splitlines() if not l.startswith('#')], p.stdout, p.stderr


rc, lst, raw, err = tool_run(lp)
check(rc == 0 and lst == ['/usr/bin/a', '/usr/libexec/b', '/usr/lib/libl.dylib',
                          '/System/Library/PrivateFrameworks/X.framework/Versions/A/X', '/usr/libexec/native'],
      'no ROOT: every image that ran, most runs first, then most translated blocks: %s' % lst)
check('1 records unreadable' in raw, 'a malformed record is counted, an unrelated line ignored: %s' % raw.splitlines()[0])
rp = os.path.join(out, 'r.json')
rc, lst, raw, err = tool_run('--root', root, '--report', rp, lp)
r = json.load(open(rp))
rows = {e['path']: e for e in r['images']}
check(rc == 0 and lst == ['/usr/bin/a', '/usr/libexec/b', '/usr/lib/libl.dylib'], '--root keeps files with an arm64 slice: %s' % lst)
check('shared-cache' in rows['/System/Library/PrivateFrameworks/X.framework/Versions/A/X']['why']
      and 'native' in rows['/usr/libexec/native']['why'], 'a shared-cache image and a native program are reported, not listed')
a, b = rows['/usr/bin/a'], rows['/usr/libexec/b']
check(a['runs'] == 3 and a['main_runs'] == 3 and a['jit_blocks'] == 40 and a['aot'] == {'aot': 1, 'none': 1, 'rejected': 1}
      and b['jit_blocks'] == 900 and b['aot'] == {'none': 1, 'unfit': 1} and r['runs'] == 7,
      'report: runs, AOT states and the largest jit count of each run (%s)' % {k: a[k] for k in ('runs', 'jit_blocks', 'aot')})
check('another build' in b.get('note', '') and 'note' not in a, 'a ROOT file with another LC_UUID is listed with a note')
rc, lst, *_ = tool_run('--root', root, '--min-runs', '2', lp)
check(lst == ['/usr/bin/a', '/usr/libexec/b'], '--min-runs 2: %s' % lst)
rc, lst, *_ = tool_run('--root', root, '--programs-only', '--base', base, lp)
check(lst == ['/usr/bin/a', '/usr/libexec/b', '/usr/sbin/extra'], '--programs-only, --base adds what is not listed yet: %s' % lst)
rc, lst, *_ = tool_run('-', stdin='\n'.join(logs[5:7]))
check(rc == 0 and lst == ['/usr/bin/a'], "'-' reads standard input")
rc, lst, raw, err = tool_run(base)
check(rc != 0 and 'no hao-use records' in err, 'a log without records fails')
sys.exit(1 if bad else 0)
