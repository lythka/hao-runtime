#!/usr/bin/env python3
"""Swift metadata initialization must see later ordinary Objective-C classes."""
import json, os, re, subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent; ROOT=HERE.parents[2]
OUT=Path(os.environ.get('OUT',ROOT/'build/objc-class-order')).resolve()
HAO=Path(os.environ.get('HAO',ROOT/'build/hao')).resolve()
OUT.mkdir(parents=True,exist_ok=True)
env={k:v for k,v in os.environ.items() if not k.startswith('HAO_')}
def command(args,name,environment=env,check=True,seconds=60):
    p=subprocess.run(['timeout','-s','KILL',str(seconds),*map(str,args)],env=environment,capture_output=True)
    (OUT/(name+'.out')).write_bytes(p.stdout);(OUT/(name+'.err')).write_bytes(p.stderr)
    if check: assert p.returncode==0,(name,p.returncode,p.stderr.decode(errors='replace')[-2000:])
    return p
for arch in ['arm64','arm64e','x86_64']:
    dest=OUT/arch;dest.mkdir(exist_ok=True)
    command(['xcrun','swiftc','-O','-target',arch+'-apple-macos27.0','-parse-as-library',
      '-module-name','ClassOrder','-import-objc-header',HERE/'Late.h','-emit-objc-header',
      '-emit-objc-header-path',dest/'Early-Swift.h','-c',HERE/'Early.swift','-o',dest/'Early.o'],'swift-'+arch)
    for name in ['Late','Main','Base']:
        command(['xcrun','clang','-O2','-fobjc-arc','-target',arch+'-apple-macos27.0',
          '-I',dest,'-c',HERE/(name+'.m'),'-o',dest/(name+'.o')],name+'-'+arch)
    for order,objects in [('swift-first',['Early','Late','Main','Base']),
                          ('objc-first',['Late','Early','Main','Base'])]:
        # Base is deliberately linked after its ObjC subclass in both orders.
        binary=OUT/(order+'-'+arch)
        command(['xcrun','swiftc','-target',arch+'-apple-macos27.0',
          *[dest/(name+'.o') for name in objects],'-framework','Foundation','-o',binary],'link-'+order+'-'+arch)
        command(['codesign','-s','-','-f',binary],'sign-'+order+'-'+arch)
results=[]
for order in ['swift-first','objc-first']:
    reference=command([OUT/(order+'-arm64')],'native-arm-'+order).stdout
    assert reference==b'entered-main\nclasses early=42 late=42\n'
    assert command([OUT/(order+'-x86_64')],'native-x86-'+order).stdout==reference
    cache=OUT/('aot-'+order);cache.mkdir(exist_ok=True)
    base=dict(env,HAO_NX_STATS='1',HAO_VERBOSE='2')
    guest=OUT/(order+'-arm64e')
    aot=command([HAO,'--aot',guest],order+'-aot-generate',dict(base,HAO_CACHE=str(cache)),check=False,seconds=120)
    for mode,args,e in [('jit',[],dict(base,HAO_NO_AOT='1')),('interp',['--interp'],dict(base,HAO_NO_AOT='1')),
                        ('aot',[],dict(base,HAO_CACHE=str(cache)))]:
        p=command([HAO,*args,guest],order+'-'+mode,e,check=False)
        loaded=mode!='aot' or bool(re.search(rb'AOT cache .* loaded',p.stderr))
        row=dict(order=order,mode=mode,rc=p.returncode,entered_main=b'entered-main' in p.stdout,
                 matches=p.returncode==0 and p.stdout==reference,aot_generated=aot.returncode==0,aot_loaded=loaded)
        results.append(row);print(row,flush=True)
(OUT/'results.json').write_text(json.dumps(results,indent=2)+'\n')
for row in results:
    if os.environ.get('EXPECT_FAILURE')=='1' and row['order']=='swift-first':
        assert not row['matches'] and not row['entered_main'],row
    else:
        assert row['matches'] and row['aot_generated'] and row['aot_loaded'],row
