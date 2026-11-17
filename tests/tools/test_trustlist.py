"""tools/hao-trust-list verdict(): the cdhash it lists and codesign's anchor check are of the same bytes (one read,
a private copy), whatever the path holds when codesign runs. A file whose bytes change between the read and the
check (an ad hoc program, then an Apple one) is not listed; an Apple program, and an Apple bundle's main executable
(its Info.plist copied beside it), are listed with their own cdhash.
  python3 -I tests/tools/test_trustlist.py ADHOC_PROGRAM APPLE_PROGRAM [APPLE_BUNDLE_EXECUTABLE]"""
import importlib.machinery, importlib.util, os, shutil, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, '..', '..', 'tools', 'hao-trust-list')
loader = importlib.machinery.SourceFileLoader('trustlist', tool)
m = importlib.util.module_from_spec(importlib.util.spec_from_loader('trustlist', loader))
loader.exec_module(m)

adhoc, apple, *bundle = sys.argv[1:]
cd = lambda p: m.cdhash(m.slice_and_arch(open(p, 'rb').read())[0])
bad = 0
with tempfile.TemporaryDirectory() as d:
    path = os.path.join(d, 'prog')
    shutil.copyfile(adhoc, path)
    real = m.subprocess.run

    def replace_first(argv, **kw):   # the path is rewritten after the read, before codesign runs
        shutil.copyfile(apple, path)
        return real(argv, **kw)
    m.subprocess.run = replace_first
    h, _, why = m.verdict(d, path)
    m.subprocess.run = real
    if h is not None:
        print('FAIL changed between read and check: listed %s (the ad hoc program is %s)' % (h, cd(adhoc))); bad += 1
    else:
        print('PASS changed between read and check: not listed (%s)' % why)
    for p in [apple] + bundle:
        h, _, why = m.verdict('/', p)
        if h is None or h != cd(p):
            print('FAIL %s: %s %s' % (p, h, why)); bad += 1
        else:
            print('PASS %s: listed, cdhash %s' % (p, h))
sys.exit(1 if bad else 0)
