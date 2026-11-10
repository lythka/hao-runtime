#!/usr/bin/env python3
"""Real disassembly -> generated aggregate unit -> native/JIT/interp/AOT CLI.

SCANNER_ROOT chooses the codesig candidate; HAO chooses the existing runtime.
The baseline scanner must fail at signature generation, before translated runs.
No Apple frameworks or graphics are loaded. OUT defaults to a fresh build dir.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
here = Path(__file__).resolve().parent
repo = here.parents[2]
scanner = Path(os.environ.get('SCANNER_ROOT', repo))
hao = Path(os.environ.get('HAO', repo/'build/hao')).resolve()
(repo/'build').mkdir(exist_ok=True)
out = Path(os.environ.get('OUT') or tempfile.mkdtemp(prefix='sret-spill-', dir=repo/'build')).resolve()
for d in ('x86', 'arm', 'if', 'aot'): (out/d).mkdir(parents=True, exist_ok=True)

def run(name, command, env=None):
    result = subprocess.run([str(x) for x in command], env=env, capture_output=True, timeout=60)
    (out/(name+'.out')).write_bytes(result.stdout)
    (out/(name+'.err')).write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name}: exit {result.returncode}; evidence {out}')
    return result.stdout, result.stderr

lib = out/'libspill.dylib'
slices = []
for arch, source, directory in [('x86_64', 'helper-x86.s', 'x86'), ('arm64e', 'helper.c', 'arm'), ('arm64', 'helper.c', '')]:
    target = out/directory/('libspill.'+arch+'.dylib')
    run('build-'+arch, ['clang', '-arch', arch, '-O1', '-dynamiclib', '-install_name', lib, here/source, '-o', target])
    slices.append(target)
run('lipo', ['lipo', '-create', *slices, '-output', lib])
run('sign-helper', ['codesign', '-s', '-', '-f', lib])
for arch in ('arm64', 'x86_64', 'arm64e'):
    run('guest-'+arch, ['clang', '-arch', arch, '-O1', here/'guest.c', '-L'+str(out), '-lspill', '-o', out/('guest.'+arch)])
    run('sign-'+arch, ['codesign', '-s', '-', '-f', out/('guest.'+arch)])

if not (scanner/'tools/codesig.py').is_file():
    print('SKIP sret-spill: no tools/codesig.py under SCANNER_ROOT=%s (the scanner is not in this tree)' % scanner)
    sys.exit(77)
spec = importlib.util.spec_from_file_location('spill_codesig', scanner/'tools/codesig.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
maps = c.build_maps(str(out/'x86'), str(out/'arm'), tempfile.mkdtemp(prefix='maps-', dir=out))
record = c.classify('_spill_bounds', *maps)
member = c.membership('_spill_bounds', *maps)
encoding, why = c.r_encoding(record, *maps)
(out/'classification.txt').write_text(repr((record, member, encoding, why))+'\n')
if not member or 'R' not in member['clauses'] or encoding != '{?=dddd}q':
    raise RuntimeError(f'scanner cannot emit spill signature: {encoding!r}, {why}; evidence {out}')
# The signature below is exclusively the scanner result, not a handwritten fix.
uuid = run('uuid', ['python3', '-I', repo/'tools/hao_ifunit.py', 'uuid', lib])[0].decode().strip()
unit = out/'unit.tsv'
unit.write_text(f'_spill_bounds\tTYPED\tA\t0x0\t-\t{encoding}\t-\n')
run('unit', ['python3', '-I', repo/'tools/hao_ifunit.py', 'write', out/'if'/uuid, '--uuid', uuid, unit])
(out/'if'/uuid).chmod(0o644)
native = run('native-arm', [out/'guest.arm64'])[0]
assert native == run('native-x86', [out/'guest.x86_64'])[0], 'native architecture outputs differ'
env = dict(os.environ, HAO_IF_DIR=str(out/'if'), HAO_VERBOSE='1', HAO_BRIDGE_STATS='1')
run('aotgen', [hao, '--aot', '--cache', out/'aot', out/'guest.arm64e'], env)
for mode in ('jit', 'interp', 'aot'):
    command = [hao] + (['--interp'] if mode == 'interp' else []) + [out/'guest.arm64e']
    stdout, stderr = run(mode, command, dict(env, HAO_CACHE=str(out/('aot' if mode == 'aot' else 'none'))))
    assert stdout == native, f'{mode}: output differs, evidence {out}'
    assert uuid.encode() in stderr and b'1 records' in stderr, f'{mode}: unit load not verified'
    if mode == 'aot':
        assert b'AOT cache ' in stderr and b' loaded' in stderr, 'AOT cache load not verified'
print(f'sret-spill execution: native ARM/x86, JIT/interp/verified AOT match; {encoding}; evidence {out}')
