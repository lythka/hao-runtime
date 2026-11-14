#!/usr/bin/env python3
"""Authored Foundation-only native NSBundle caller; no GUI app/framework target."""
from pathlib import Path
import os,plistlib,subprocess
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=Path(os.environ.get('OUT',ROOT/'build/native-bundle')).resolve()
HAO=Path(os.environ.get('HAO',ROOT/'build/hao')).resolve()
OUT.mkdir(parents=True,exist_ok=True)
def run(args,**kw): return subprocess.run(list(map(str,args)),check=True,timeout=60,**kw)
def sign(path):run(['codesign','-f','-s','-',path],capture_output=True)
for name,arches in [('GuestBundleFixture',['arm64','arm64e']),('UniversalBundleFixture',['arm64','arm64e','x86_64'])]:
 bundle=OUT/(name+'.bundle');exe=bundle/'Contents/MacOS'/name
 exe.parent.mkdir(parents=True,exist_ok=True)
 resources=bundle/'Contents/Resources';resources.mkdir(exist_ok=True)
 (resources/'payload.txt').write_text('resource\n')
 (bundle/'Contents/Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='test.hao.'+name,CFBundleExecutable=name,CFBundlePackageType='BNDL',NSPrincipalClass=name)))
 run(['clang',*[v for a in arches for v in ['-arch',a]],'-fobjc-arc','-bundle','-framework','Foundation','-DCLASS='+name,HERE/'plugin.m','-o',exe]);sign(bundle)
lib=OUT/'libBundleCaller.dylib'
run(['clang','-arch','arm64','-arch','arm64e','-arch','x86_64','-fobjc-arc','-dynamiclib','-framework','Foundation',HERE/'helper.m','-install_name',lib,'-o',lib]);sign(lib)
for arch in ['arm64','arm64e']:
 exe=OUT/('guest.'+arch)
 run(['clang','-arch',arch,HERE/'main.c','-L',OUT,'-lBundleCaller','-o',exe]);sign(exe)
for name in ['GuestBundleFixture','UniversalBundleFixture']:
 bundle=OUT/(name+'.bundle')
 native=run([OUT/'guest.arm64',bundle],capture_output=True,text=True)
 env={**os.environ,'HAO_CACHE':str(OUT/'aot'),'HAO_VERBOSE':'1'}
 env.pop('HAO_NO_AOT',None)
 run([HAO,'--aot','--cache',OUT/'aot',OUT/'guest.arm64e'],capture_output=True,env=env)
 run([HAO,'--aot','--cache',OUT/'aot',bundle/'Contents/MacOS'/name],capture_output=True,env=env)
 for mode in ['jit','interp','aot']:
  mode_env=dict(env)
  if mode != 'aot':mode_env['HAO_NO_AOT']='1'
  if name=='GuestBundleFixture':mode_env['EXPECT_GUEST_BUNDLE']='1'
  result=subprocess.run([str(HAO),*(['--interp'] if mode=='interp' else []),str(OUT/'guest.arm64e'),str(bundle)],capture_output=True,text=True,timeout=60,env=mode_env)
  (OUT/(name+'-'+mode+'.log')).write_text(result.stdout+result.stderr)
  assert result.returncode==0 and result.stdout==native.stdout,(name,mode,result.returncode,result.stdout,result.stderr)
  if mode=='aot':assert result.stderr.count('AOT cache ') >= (2 if name=='GuestBundleFixture' else 1) and ' loaded' in result.stderr
  print(name,mode,'PASS')
# Corrupt only one signed slice. A native validation failure in a universal
# image must never become permission to run its still-valid ARM slice.
import shutil,struct
for name,cpu in [('GuestBundleFixture',0x100000c),('UniversalBundleFixture',0x1000007)]:
 source=OUT/(name+'.bundle')/'Contents/MacOS'/name
 dst=OUT/(name+'-bad-signature');shutil.copyfile(source,dst)
 data=bytearray(dst.read_bytes());magic,count=struct.unpack_from('>II',data)
 assert magic==0xcafebabe
 for i in range(count):
  c,sub,off,size,align=struct.unpack_from('>IIIII',data,8+20*i)
  if c==cpu:
   if cpu==0x1000007:
    # Native dyld refuses an MH_EXECUTE supplied to dlopen. Its ARM slice
    # remains valid; fallback must still refuse this universal file.
    struct.pack_into('<I',data,off+12,2)
   else:
    marker=data.find(b'hao-bundle-signature-marker-83721',off,off+size)
    assert marker>=0;data[marker]^=1
 dst.write_bytes(data)
 for mode in ['jit','interp','aot']:
  env={**os.environ,'HAO_CACHE':str(OUT/'aot')}
  if mode!='aot':env['HAO_NO_AOT']='1'
  result=subprocess.run(list(map(str,[HAO,*(['--interp'] if mode=='interp' else []),OUT/'guest.arm64e',dst,'reject'])),capture_output=True,text=True,env=env,timeout=60)
  # macOS may kill the reader when a cached signed page is corrupted; either
  # kernel enforcement or an explicit loader refusal prevents execution.
  assert (result.returncode==0 and result.stdout=='refused 1\n') or (result.returncode==-9 and not result.stdout),(name,mode,result.returncode,result.stdout,result.stderr)
  print(name,'invalid signed slice',mode,'PASS')
# An ARM-only dylib a native caller dlopens (NSBundle loading a framework) loads as a guest image too.
dylib=OUT/'arm-only.dylib'
run(['clang','-arch','arm64','-arch','arm64e','-dynamiclib','-fobjc-arc','-framework','Foundation',HERE/'plugin.m','-o',dylib]);sign(dylib)
result=subprocess.run(list(map(str,[HAO,OUT/'guest.arm64e',dylib,'reject'])),capture_output=True,text=True,timeout=60)
assert result.stdout=='refused 0\n',(result.returncode,result.stdout,result.stderr)
print('ARM-only dylib from a native caller loads PASS')
