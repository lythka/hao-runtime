#!/usr/bin/env python3
"""Legacy pointer fixups: own CLI and two guest dylibs versus native ARM."""
import json
import os
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = Path(os.environ.get('OUT', ROOT/'build/legacy-fixups')).resolve()
HAO = Path(os.environ.get('HAO', ROOT/'build/hao')).resolve()
OUT.mkdir(parents=True, exist_ok=True)
env = {k:v for k,v in os.environ.items() if not k.startswith('HAO_')}

def command(args, policy=env):
    return subprocess.run(list(map(str,args)), env=policy, capture_output=True, timeout=60, check=True)

def legacy(path):
    data = path.read_bytes()
    offset = 32
    sizes = None
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        op, size = struct.unpack_from('<II',data,offset)
        assert op != 0x80000034, 'fixture unexpectedly uses chained fixups'
        if op in (0x22,0x80000022):
            fields = struct.unpack_from('<12I',data,offset)
            sizes = fields[3::2]
        offset += size
    assert sizes is not None, 'fixture lacks legacy dyld streams'
    return sizes

for arch in ('arm64','x86_64'):
    folder=OUT/arch
    folder.mkdir(exist_ok=True)
    common=['xcrun','clang','-arch',arch,'-O1','-mmacosx-version-min=11.0','-Wl,-no_fixup_chains']
    command(common+['-dynamiclib','-install_name','@rpath/liba.dylib',HERE/'a.c','-o',folder/'liba.dylib'])
    command(common+['-dynamiclib','-install_name','@rpath/libb.dylib','-L'+str(folder),'-la',HERE/'b.c','-o',folder/'libb.dylib'])
    command(common+['-Wl,-rpath,@executable_path','-Wl,-U,_hao_legacy_missing','-L'+str(folder),'-la','-lb',HERE/'main.c','-o',folder/'main'])
    for binary in ('liba.dylib','libb.dylib','main'):
        command(['codesign','-f','-s','-',folder/binary])
        sizes=legacy(folder/binary)
        if binary!='main': assert sizes[2], 'dylib must exercise weak-bind definitions'
        if binary!='liba.dylib': assert sizes[1] and sizes[3], 'must exercise bind and lazy-bind'

reference=command([OUT/'arm64/main']).stdout
expected=b'rebase=37 addend=29 indirect=22 calls=34,37 missing=1 weak_values=23,23 weak_identity=1 stable_identity=1\n'
assert reference==expected, reference
cache=OUT/'aot'
cache.mkdir(exist_ok=True)
aot_env=dict(env,HAO_CACHE=str(cache),HAO_VERBOSE='1')
for binary in ('liba.dylib','libb.dylib','main'):
    p=command([HAO,'--aot','--cache',cache,OUT/'arm64'/binary],aot_env)
    (OUT/('aot-build-'+binary+'.log')).write_bytes(p.stdout+p.stderr)
rows=[]
for mode,args,policy in [
    ('native-arm',[OUT/'arm64/main'],env),
    ('native-x86',[OUT/'x86_64/main'],env),
    ('jit',[HAO,OUT/'arm64/main'],dict(env,HAO_NO_AOT='1')),
    ('interp',[HAO,'--interp',OUT/'arm64/main'],dict(env,HAO_NO_AOT='1')),
    ('aot',[HAO,OUT/'arm64/main'],aot_env)]:
    p=command(args,policy)
    (OUT/(mode+'.out')).write_bytes(p.stdout)
    (OUT/(mode+'.err')).write_bytes(p.stderr)
    loads=sum(b'AOT cache ' in line and b' loaded' in line for line in p.stderr.splitlines())
    ok=p.stdout==reference and (mode!='aot' or loads==3)
    rows.append(dict(mode=mode,matches=ok,aot_loaded=loads if mode=='aot' else None))
    print(mode,'PASS' if ok else 'FAIL',flush=True)
(OUT/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
raise SystemExit(not all(row['matches'] for row in rows))
