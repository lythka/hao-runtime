#!/usr/bin/env python3
"""Author two equivalent ABI keypath patterns using direct/indirect IDs."""
import json
import os
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = Path(os.environ.get('OUT', ROOT/'build/keypath-identity')).resolve()
HAO = Path(os.environ.get('HAO', ROOT/'build/hao')).resolve()
OUT.mkdir(parents=True, exist_ok=True)


def run(args, **kw):
    return subprocess.run(list(map(str, args)), check=True, timeout=60, **kw)


def patch(path, target, slot):
    data = bytearray(path.read_bytes())
    ncmds, _ = struct.unpack_from('<II', data, 16)
    pos, sections = 32, []
    for _ in range(ncmds):
        cmd, width = struct.unpack_from('<II', data, pos)
        if cmd == 0x19:
            count = struct.unpack_from('<I', data, pos+64)[0]
            for i in range(count):
                p = pos+72+80*i
                name = data[p:p+16].rstrip(b'\0').decode()
                segment = data[p+16:p+32].rstrip(b'\0').decode()
                address, size, offset = struct.unpack_from('<QQI', data, p+32)
                sections.append((segment, name, address, size, offset))
        pos += width
    found = []
    for segment, name, address, size, offset in sections:
        if segment != '__TEXT' or name != '__const': continue
        for i in range(0, size-24, 4):
            header, component = struct.unpack_from('<II', data, offset+i)
            # A single, settable pointer-ID computed component without args.
            if header != 0x80000010 or component & ~0xf != 0x02400000: continue
            found.append((address+i+4, offset+i+4))
    assert len(found) == 2, found
    for index, (address, offset) in enumerate(found):
        struct.pack_into('<I', data, offset, 0x02400000 | (2 if index else 0))
        destination = slot if index else target
        struct.pack_into('<i', data, offset+4, destination-(address+4))
    path.write_bytes(data)
    return [{'header': hex(a), 'resolution': 'indirect' if i else 'direct'} for i,(a,_) in enumerate(found)]


records = []
for arch in ('arm64', 'arm64e', 'x86_64'):
    obj = OUT/('probe.'+arch+'.o')
    run(['xcrun', 'swiftc', '-O', '-module-name', 'KeyPathIdentity', '-target', arch+'-apple-macos27.0', '-emit-object', HERE/'Probe.swift', '-o', obj])
    symbols = run(['nm', '-g', obj], capture_output=True, text=True).stdout
    getter = next(line.split()[-1] for line in symbols.splitlines() if line.endswith('RootC5firstSivg'))
    assembly = OUT/('slot.'+arch+'.s')
    assembly.write_text('.section __DATA,__data\n.p2align 3\n.globl _keypath_identity_slot\n_keypath_identity_slot:\n.quad '+getter+'\n')
    slotobj = OUT/('slot.'+arch+'.o')
    run(['clang', '-arch', arch, '-mmacosx-version-min=27.0', '-c', assembly, '-o', slotobj])
    binary = OUT/('probe.'+arch)
    run(['xcrun', 'swiftc', '-target', arch+'-apple-macos27.0', obj, slotobj, '-o', binary])
    symbols = run(['nm', '-n', binary], capture_output=True, text=True).stdout
    addresses = {p[-1]: int(p[0],16) for line in symbols.splitlines() if len(p:=line.split()) == 3 and p[0][0] in '0123456789abcdef'}
    patches = patch(binary, addresses[getter], addresses['_keypath_identity_slot'])
    run(['codesign', '-f', '-s', '-', binary], capture_output=True)
    records.append({'arch': arch, 'patches': patches})
(OUT/'patterns.json').write_text(json.dumps(records, indent=2)+'\n')
expected = 'read 73 73\nequal true\nhash true\n'
for arch in ('arm64', 'x86_64'):
    result = run([OUT/('probe.'+arch)], capture_output=True, text=True)
    (OUT/('native-'+arch+'.out')).write_text(result.stdout)
    assert result.stdout == expected, result.stdout
    print(arch, 'PASS', flush=True)
cache = OUT/'aot'
cache.mkdir(mode=0o700, exist_ok=True)
run([HAO, '--aot', '--cache', cache, OUT/'probe.arm64e'], capture_output=True)
for mode, flags in [('jit', []), ('interp', ['--interp']), ('aot', ['-v']), ('no-fixup', [])]:
    env = {**os.environ, **({'HAO_CACHE':str(cache)} if mode=='aot' else {'HAO_NO_AOT':'1'})}
    if mode == 'no-fixup': env['HAO_NO_FIXUP_THUNKS'] = '1'
    result = subprocess.run([HAO, *flags, OUT/'probe.arm64e'], capture_output=True, text=True, timeout=60, env=env)
    (OUT/(mode+'.out')).write_text(result.stdout)
    (OUT/(mode+'.err')).write_text(result.stderr)
    assert result.returncode == 0 and result.stdout == expected, (mode, result.returncode, result.stdout, result.stderr)
    if mode == 'aot': assert 'AOT cache ' in result.stderr and ' loaded:' in result.stderr
    print(mode, 'PASS', flush=True)
base = os.environ.get('HAO_BASE')
if base:
    for mode, flags in [('jit', []), ('interp', ['--interp'])]:
        result = subprocess.run([base, *flags, OUT/'probe.arm64e'], capture_output=True, text=True, timeout=60, env={**os.environ, 'HAO_NO_AOT':'1'})
        (OUT/('base-'+mode+'.out')).write_text(result.stdout)
        (OUT/('base-'+mode+'.err')).write_text(result.stderr)
        assert result.returncode == 0 and result.stdout == 'read 73 73\nequal false\nhash false\n', result
        print('baseline-'+mode, 'REPRODUCED', flush=True)
