"""--cryptex NAME=DIR (tools/hao_root.py): a System root with a cryptex mounted on its own gives the packaging tools'
outputs byte for byte as the same root with the cryptex in place at System/Volumes/Preboot/Cryptexes/NAME. Two trees
of the same files: graft/ (the cryptex inside, System/Cryptexes/OS a link to it, as on the installed system) and
sys/ + os/ (the cryptex beside it). Compared: hao-trust-list (the list, its log and --stubs), hao-embed --stubs
--scan (manifest and stubs; its log with the cryptex folder named by its place under ROOT, as hao-image-payload
records it) and stub_identity_gate.py, and hao_root's paths for a link through System/Cryptexes.
  python3 -I tests/tools/test_root.py OUT PROGRAM APPLE_UNIVERSAL STUB X86_CACHE
PROGRAM is an arm64e-only program (ad hoc signed), APPLE_UNIVERSAL a program whose arm64e slice Apple signed."""
import filecmp, os, shutil, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
tools = os.path.join(here, '..', '..', 'tools')
sys.path.insert(0, tools)
import hao_root  # noqa: E402

out, prog, apple, stub, x86 = sys.argv[1:]
bad = 0


def check(ok, what):
    global bad
    print(('PASS ' if ok else 'FAIL ') + what)
    bad += not ok


shutil.rmtree(out, ignore_errors=True)
graft, sysroot, cx = (os.path.join(out, n) for n in ('graft', 'sys', 'os'))
for top, inner in ((graft, os.path.join(graft, hao_root.CRYPTEXES, 'OS')), (sysroot, cx)):
    os.makedirs(os.path.join(top, 'System/Library/CoreServices'))
    shutil.copy('/System/Library/CoreServices/SystemVersion.plist', os.path.join(top, 'System/Library/CoreServices'))
    os.makedirs(os.path.join(top, 'System/Cryptexes'))
    os.makedirs(os.path.join(top, 'System/Volumes/Preboot'), exist_ok=True)
    os.symlink('../../System/Volumes/Preboot/Cryptexes/OS', os.path.join(top, 'System/Cryptexes/OS'))
    os.makedirs(os.path.join(top, 'usr/bin'))
    shutil.copy(prog, os.path.join(top, 'usr/bin/zprog'))
    os.makedirs(os.path.join(inner, 'usr/bin'))
    os.makedirs(os.path.join(inner, 'usr/libexec'))
    shutil.copy(prog, os.path.join(inner, 'usr/libexec/aprog'))
    subprocess.run(['lipo', '-thin', 'arm64e', apple, '-output', os.path.join(inner, 'usr/bin/applethin')], check=True)
    os.symlink('../bin/applethin', os.path.join(inner, 'usr/libexec/link'))
cxa = ['--cryptex', 'OS=' + cx]

t = hao_root.Root(sysroot, {'OS': cx})
via = os.path.join(sysroot, 'System/Cryptexes/OS/usr/libexec/link')
check(t.relpath(via) == 'System/Volumes/Preboot/Cryptexes/OS/usr/bin/applethin' and
      t.file(via) == os.path.realpath(os.path.join(cx, 'usr/bin/applethin')),
      'hao_root: a link through System/Cryptexes resolves into the cryptex folder, recorded under ROOT')
check(hao_root.Root(graft).relpath(os.path.join(graft, 'System/Cryptexes/OS/usr/libexec/link')) ==
      t.relpath(via), 'hao_root: the same path as with the cryptex in place')


def run(argv, name):
    r = subprocess.run([sys.executable, '-I'] + argv, capture_output=True, text=True)
    open(os.path.join(out, name), 'w').write(r.stdout + r.stderr)
    return r.returncode


tl = os.path.join(tools, 'hao-trust-list')
rc = (run([tl, graft], 'trust-graft'), run([tl] + cxa + [sysroot], 'trust-sys'))
same = filecmp.cmp(os.path.join(out, 'trust-graft'), os.path.join(out, 'trust-sys'), shallow=False)
check(rc == (0, 0) and same and 'System/Volumes/Preboot/Cryptexes/OS/usr/bin/applethin' in open(os.path.join(out, 'trust-sys')).read(),
      'hao-trust-list: the same list and log with the cryptex beside ROOT')

emb = os.path.join(tools, 'hao-embed')
common = ['--stub', stub, '--x86-cache', x86, '--scan']
rc = (run([emb, '--stubs', os.path.join(out, 'e-graft'), '--root', graft] + common, 'embed-graft'),
      run([emb, '--stubs', os.path.join(out, 'e-sys'), '--root', sysroot] + cxa + common, 'embed-sys'))
log = {k: open(os.path.join(out, 'embed-' + k)).read() for k in ('graft', 'sys')}
log['graft'] = log['graft'].replace(graft + '/', '')
for d in (os.path.realpath(cx), cx):   # as hao-image-payload maps the log: either spelling of the folder
    log['sys'] = log['sys'].replace(d + '/', hao_root.CRYPTEXES + '/OS/')
log['sys'] = log['sys'].replace(sysroot + '/', '')
d = filecmp.dircmp(os.path.join(out, 'e-graft'), os.path.join(out, 'e-sys'))
stubs = filecmp.dircmp(os.path.join(out, 'e-graft/stubs'), os.path.join(out, 'e-sys/stubs'))
check(rc == (0, 0) and log['graft'] == log['sys'] and 'links 0 (not 3)' not in log['sys'] and log['sys'].count('\nok\t') + log['sys'].startswith('ok\t') == 3 and
      not d.diff_files and not d.left_only and not d.right_only and not stubs.diff_files and not stubs.left_only and
      not stubs.right_only and len(stubs.same_files) == 3,
      'hao-embed --stubs --scan: the same manifest, stubs and log (3 programs, 2 in the cryptex)')

gate = os.path.join(tools, 'stub_identity_gate.py')
rc = (run([gate, '--x86-cache', x86, '--stubs', os.path.join(out, 'e-graft'), '--root', graft, '--out',
           os.path.join(out, 'g-graft')], 'gate-graft'),
      run([gate, '--x86-cache', x86, '--stubs', os.path.join(out, 'e-sys'), '--root', sysroot] + cxa +
          ['--out', os.path.join(out, 'g-sys')], 'gate-sys'))
g = filecmp.dircmp(os.path.join(out, 'g-graft'), os.path.join(out, 'g-sys'))
check(rc == (0, 0) and not g.diff_files and len(g.same_files) == 3, 'stub_identity_gate.py: the same verdicts')

tg = os.path.join(out, 'trusted')
open(tg, 'w').write(open(os.path.join(out, 'trust-sys')).read())
rc = (run([tl, '--stubs', os.path.join(out, 'e-graft'), tg, graft], 'ids-graft'),
      run([tl] + cxa + ['--stubs', os.path.join(out, 'e-sys'), tg, sysroot], 'ids-sys'))
check(rc == (0, 0) and filecmp.cmp(os.path.join(out, 'ids-graft'), os.path.join(out, 'ids-sys'), shallow=False) and
      'System/Volumes/Preboot/Cryptexes/OS/usr/bin/applethin' in open(os.path.join(out, 'ids-sys')).read(),
      'hao-trust-list --stubs: the same stub identities')
sys.exit(1 if bad else 0)
