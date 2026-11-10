#!/usr/bin/env python3
"""Native calls of aggregate methods on a class realized after image registration."""
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = Path(os.environ.get('OUT', ROOT / 'build/late-method-signatures')).resolve()
HAO = Path(os.environ.get('HAO', ROOT / 'build/hao')).resolve()
if 'ABSOLUTE_METHODS' not in os.environ:
    # dyld cache optimization and linker choices can retain either method
    # representation. Exercise both, including the fnslots ordering case.
    results = []
    for name, flag in [('relative', '0'), ('absolute', '1')]:
        policy = dict(os.environ, ABSOLUTE_METHODS=flag, OUT=str(OUT/name))
        results.append(subprocess.run(['python3', str(__file__)], env=policy).returncode)
    raise SystemExit(any(results))
ABSOLUTE = os.environ.get('ABSOLUTE_METHODS') == '1'
OUT.mkdir(parents=True, exist_ok=True)

def build(args):
    subprocess.run(list(map(str,args)),check=True)

for arch in ('arm64', 'arm64e', 'x86_64'):
    build(['xcrun','clang','-arch',arch,'-mmacosx-version-min=27.0','-O2','-dynamiclib',HERE/'Native.m',
           '-framework','Foundation','-install_name',OUT/'libRectProbe.dylib','-o',OUT/(arch+'.dylib')])
    module = OUT/arch
    module.mkdir(exist_ok=True)
    build(['xcrun','swiftc','-O','-target',arch+'-apple-macos27.0','-enable-library-evolution',
           '-emit-library','-emit-module','-module-name','ResilientProbe',
           '-emit-module-path',module/'ResilientProbe.swiftmodule',HERE/'Resilient.swift',
           '-Xlinker','-install_name','-Xlinker',OUT/'libResilientProbe.dylib',
           '-o',module/'libResilientProbe.dylib'])
build(['lipo','-create',*(OUT/(a+'.dylib') for a in ('arm64','arm64e','x86_64')),'-output',OUT/'libRectProbe.dylib'])
build(['codesign','-s','-','-f',OUT/'libRectProbe.dylib'])
build(['lipo','-create',*(OUT/a/'libResilientProbe.dylib' for a in ('arm64','arm64e','x86_64')),
       '-output',OUT/'libResilientProbe.dylib'])
build(['codesign','-s','-','-f',OUT/'libResilientProbe.dylib'])
for arch in ('arm64','arm64e','x86_64'):
    build(['xcrun','swiftc','-O','-target',arch+'-apple-macos27.0','-import-objc-header',HERE/'Native.h',
           HERE/'Guest.swift','-I',OUT/arch,'-L',OUT,'-lRectProbe','-lResilientProbe',
           *(['-Xlinker','-no_objc_relative_method_lists'] if ABSOLUTE else []),'-o',OUT/('guest.'+arch)])
    build(['codesign','-s','-','-f',OUT/('guest.'+arch)])
env = {k:v for k,v in os.environ.items() if not k.startswith('HAO_')}
env['HAO_NO_AOT']='1'
cache=OUT/'aot'
cache.mkdir(exist_ok=True)
aot_env=dict(env, HAO_CACHE=str(cache), HAO_VERBOSE='1')
aot_env.pop('HAO_NO_AOT')
p=subprocess.run(['timeout','-s','KILL','60',str(HAO),'--aot',str(OUT/'guest.arm64e')],env=aot_env,capture_output=True)
(OUT/'aot-build.log').write_bytes(p.stdout+p.stderr)
if p.returncode: raise SystemExit('AOT generation failed; see aot-build.log')
rows=[]
for mode,args,policy in [
    ('arm',[OUT/'guest.arm64'],env),
    ('native-x86',[OUT/'guest.x86_64'],env),
    ('jit',[HAO,OUT/'guest.arm64e'],env),
    ('interp',[HAO,'--interp',OUT/'guest.arm64e'],env),
    ('aot',[HAO,OUT/'guest.arm64e'],aot_env),
    ('jit-no-fixup-thunks',[HAO,OUT/'guest.arm64e'],dict(env,HAO_NO_FIXUP_THUNKS='1'))]:
    p=subprocess.run(['timeout','-s','KILL','30',*map(str,args)],env=policy,capture_output=True)
    (OUT/(mode+'.out')).write_bytes(p.stdout); (OUT/(mode+'.err')).write_bytes(p.stderr)
    loaded=mode!='aot' or any(b'AOT cache ' in line and b' loaded' in line for line in p.stderr.splitlines())
    ok=p.returncode==0 and p.stdout==b'rectangles preserved\n'*3+b'frame preserved\n'*3 and loaded
    rows.append(dict(mode=mode,rc=p.returncode,matches=ok,aot_loaded=loaded if mode=='aot' else None))
    print(mode,p.returncode,p.stdout.decode(errors='replace').strip(),'PASS' if ok else 'FAIL',flush=True)
(OUT/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
raise SystemExit(not all(row['matches'] for row in rows))
