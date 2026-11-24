#!/usr/bin/env python3
"""Authored Foundation-only native NSBundle caller; no GUI app/framework target."""
from pathlib import Path
import os,plistlib,subprocess,platform,signal
# Keep the authored helper native to exercise the bridge.
os.environ["HAO_NATIVE_APP_LIBS"] = "1"

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

# Foundation opens a guest app's universal plug-in, whose dependency has
# only an ARM slice at run time. The native slice cannot load that dependency.
contents = OUT / 'Fixture.app/Contents'
main = contents / 'MacOS/Fixture'
bundle = contents / 'PlugIns/AppPlugin.bundle'
plugin = bundle / 'Contents/MacOS/AppPlugin'
main.parent.mkdir(parents=True, exist_ok=True)
plugin.parent.mkdir(parents=True, exist_ok=True)
(bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
    CFBundleIdentifier='test.hao.AppPlugin', CFBundleExecutable='AppPlugin',
    CFBundlePackageType='BNDL', NSPrincipalClass='AppPlugin')))
dep = plugin.parent / 'libRequired.dylib'
for arches in [['-arch', 'arm64', '-arch', 'x86_64'], ['-arch', 'arm64']]:
    run(['clang', *arches, '-dynamiclib', '-install_name', '@loader_path/' + dep.name,
         '-x', 'c', '-', '-o', dep], input=b'int required(void) { return 1; }\n')
    sign(dep)
    if 'x86_64' in arches:
        run(['clang', *arches, '-bundle', '-framework', 'Foundation', '-DCLASS=AppPlugin',
             HERE / 'plugin.m', '-Wl,-needed_library,' + str(dep), '-o', plugin])
sign(bundle)
run(['clang', '-arch', 'arm64', '-framework', 'Foundation', HERE / 'app.m', '-o', main])
sign(main)
env = dict(os.environ)
env.pop('HAO_NATIVE_APP_LIBS', None)
run([main, bundle], env=env)
run([HAO, main, bundle], env=env)
print("guest app's universal plug-in with ARM-only dependency PASS")

# A nested app owns its own plug-ins. A sibling plug-in in its outer app
# keeps native loader semantics, including dyld's architecture refusal.
nested = contents / 'Helpers/Nested.app/Contents/MacOS/Nested'
nested.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(main, nested)
def expect_native_refusal(executable, plugin_bundle):
    result = subprocess.run([HAO, executable, plugin_bundle], env=env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 1 and 'incompatible architecture' in result.stderr, result

expect_native_refusal(nested, bundle)
print("nested app preserves native loading of outer app's plug-in PASS")

nested_bundle = nested.parent.parent / 'PlugIns/AppPlugin.bundle'
shutil.copytree(bundle, nested_bundle, dirs_exist_ok=True)
run([HAO, nested, nested_bundle], env=env)
expect_native_refusal(main, nested_bundle)
print("outer app preserves native loading of nested app's plug-in PASS")

alias = nested.parent / 'OuterExecutable'
alias.unlink(missing_ok=True)
alias.symlink_to(main)
run([HAO, alias, bundle], env=env)
expect_native_refusal(alias, nested_bundle)
print("an executable symlink preserves its owning app PASS")

# A prepared outer domain explicitly selects its guest libraries from all
# embedded member processes, including its nested apps.
for program in [main, nested]:
    sign(program)
    run([ROOT / 'tools/hao-embed', '--stub', HAO.parent / 'hao-stub', '--host-test',
         '--arm-app-member', program, '--in-place', program], capture_output=True)
domain_env = env
for program, target in [(main, nested_bundle), (nested, bundle)]:
    run(['/usr/bin/arch', '-x86_64', program, target], env=domain_env)
profile = OUT / 'selected-app.sb'
profile.write_text('(version 1) (allow default) (deny file-read* (subpath "/private/var/db/hao/app-domains"))')
run(['/usr/bin/sandbox-exec', '-f', profile, '/usr/bin/arch', '-x86_64', nested, bundle], env=domain_env)
print('selected nested app loads its outer guest plugin under a policy-store read denial PASS')
print('opted-in embedded apps load their outer and nested guest plug-ins PASS')

contents_copy = OUT / 'Unselected.app/Contents'
outside = contents_copy / 'PlugIns/AppPlugin.bundle'
shutil.copytree(bundle, outside, dirs_exist_ok=True)
escaped = nested.parent / 'Outside.bundle'
escaped.unlink(missing_ok=True)
escaped.symlink_to(outside)
refused = subprocess.run(['/usr/bin/arch', '-x86_64', nested, escaped], env=domain_env,
                         capture_output=True, text=True, timeout=60)
assert refused.returncode == 0, refused  # an unverified native closure takes the validated ARM route
outside_executable = outside / 'Contents/MacOS/AppPlugin'
thin = OUT / 'outside.x86_64'
run(['lipo', '-thin', 'x86_64', outside_executable, '-output', thin])
shutil.copyfile(thin, outside_executable)
sign(outside)
refused = subprocess.run(['/usr/bin/arch', '-x86_64', nested, escaped], env=domain_env,
                         capture_output=True, text=True, timeout=60)
assert refused.returncode == 1, refused

moved = main.parent / 'RelocatedFixture'
shutil.copy2(main, moved)
refused = subprocess.run(['/usr/bin/arch', '-x86_64', moved, bundle], env=domain_env,
                         capture_output=True, text=True, timeout=60)
assert refused.returncode == 126 and 'prepared path' in refused.stderr, refused
print('selected ARM app rejects relocated members and x86-only disk libraries PASS')

# Two independent entry points request the same universal closure by alias.
# Both implementations print their own constructor and return distinct values.
libs = contents / "Frameworks"
libs.mkdir(exist_ok=True)
alias_dir = contents / "SelectionAlias"
alias_dir.symlink_to(libs)
selection = HERE / "selection.c"
leaf = libs / "Leaf.dylib"
branch = libs / "Branch.dylib"
arches = ["-arch", "arm64", "-arch", "x86_64"]
for image, role, dependencies in [(leaf, "LEAF", []), (branch, "BRANCH", [leaf])]:
    run(["clang", *arches, "-dynamiclib", "-D" + role, selection, *dependencies,
         "-install_name", alias_dir / image.name, "-o", image])
    sign(image)
clients = []
for name, extra in [("linked", ["-DLINKED", branch]), ("dynamic", [])]:
    client = main.parent / name
    run(["clang", *arches, selection, *extra, "-o", client])
    sign(client)
    run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
         "--arm-app-member", client, "--in-place", client], capture_output=True)
    clients.append(client)

def selected(client, extra=()):
    return subprocess.run(list(map(str, ["/usr/bin/arch", "-x86_64", *extra, client,
                                         alias_dir / branch.name])),
                          capture_output=True, text=True, timeout=60, env=env)

for client in clients:
    result = selected(client)
    assert result.returncode == 0 and result.stdout == "result 11\n", result
    assert "branch-arm" in result.stderr and "leaf-arm" in result.stderr and "-intel" not in result.stderr, result

spawner = OUT / "spawn-selected"
run(["clang", "-DSPAWNER", selection, "-o", spawner])
sign(spawner)
spawned = run([spawner, clients[1], alias_dir / branch.name], capture_output=True, text=True, env=env)
assert spawned.stdout == "result 11\n" and "-intel" not in spawned.stderr, spawned

# Host bootstrap restrictions must leave original guest runpath policy intact.
rpath_branch = libs / "RPathBranch.dylib"
run(["clang", *arches, "-dynamiclib", "-DBRANCH", selection, leaf,
     "-install_name", "@rpath/RPathBranch.dylib", "-o", rpath_branch])
sign(rpath_branch)
for name, rpath in [("executable-rpath", "@executable_path/../SelectionAlias"),
                    ("loader-rpath", "@loader_path/../SelectionAlias")]:
    runpath_client = main.parent / name
    run(["clang", *arches, "-DLINKED", selection, rpath_branch, "-Wl,-rpath," + rpath,
         "-o", runpath_client])
    sign(runpath_client)
    run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
         "--arm-app-member", runpath_client, "--in-place", runpath_client], capture_output=True)
    resolved = selected(runpath_client)
    assert resolved.returncode == 0 and resolved.stdout == "result 11\n", (rpath, resolved)

# A leaf-name dlopen uses the caller's LC_RPATH, even from an unrelated cwd.
leaf_client = main.parent / "leaf-search"
run(["clang", *arches, selection, "-Wl,-rpath,@executable_path/../SelectionAlias", "-o", leaf_client])
sign(leaf_client)
native_leaf = run([leaf_client, branch.name], cwd=OUT, capture_output=True, text=True)
assert native_leaf.stdout == "result 11\n", native_leaf
run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
     "--arm-app-member", leaf_client, "--in-place", leaf_client], capture_output=True)
selected_leaf = run(["/usr/bin/arch", "-x86_64", leaf_client, branch.name], cwd=OUT,
                    capture_output=True, text=True)
assert selected_leaf.stdout == "result 11\n" and "-intel" not in selected_leaf.stderr, selected_leaf

# Only the guest opener dylib carries the runpath needed by its plugin's dependency.
layer = libs / "Layer"
layer.mkdir()
hidden = layer / "Hidden.dylib"
plugin_with_dependency = layer / "Plugin.dylib"
opener = libs / "Opener.dylib"
for path, role, extra, identifier in [
        (hidden, "LEAF", [], "@rpath/Hidden.dylib"),
        (plugin_with_dependency, "BRANCH", [hidden], "@rpath/Plugin.dylib"),
        (opener, "OPENER", ["-Wl,-rpath,@loader_path/Layer"], "@rpath/Opener.dylib")]:
    run(["clang", *arches, "-dynamiclib", "-D" + role, selection, *extra,
         "-install_name", identifier, "-o", path])
    sign(path)
for argv in [[clients[1], opener], ["/usr/bin/arch", "-x86_64", clients[1], opener]]:
    through_caller = run(argv, cwd=OUT, capture_output=True, text=True)
    assert through_caller.stdout == "result 11\n" and "-intel" not in through_caller.stderr, through_caller

restricted_relative = main.parent / "restricted-relative"
run(["clang", *arches, selection, "-Wl,-sectcreate,__RESTRICT,__restrict,/dev/null", "-o", restricted_relative])
sign(restricted_relative)
run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
     "--arm-app-member", restricted_relative, "--in-place", restricted_relative], capture_output=True)
relative_refused = subprocess.run(["/usr/bin/arch", "-x86_64", restricted_relative, "./Branch.dylib"],
                                  cwd=libs, capture_output=True, text=True, timeout=60)
assert (relative_refused.returncode == 3 and "relative path not allowed" in relative_refused.stderr
        and "-intel" not in relative_refused.stderr), relative_refused

# Guest dyld enumeration must see the mapped ARM file, not just native dyld's images.
image_observer = main.parent / "image-observer"
run(["clang", *arches, "-DIMAGE_OBSERVER", selection, "-o", image_observer])
sign(image_observer)
native_images = run([image_observer, branch], capture_output=True, text=True)
unselected_images = subprocess.run([HAO, image_observer, branch], capture_output=True, text=True,
                                   timeout=60, env=env)
assert unselected_images.returncode == 5, unselected_images
run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
     "--arm-app-member", image_observer, "--in-place", image_observer], capture_output=True)
translated_images = run(["/usr/bin/arch", "-x86_64", image_observer, branch],
                         capture_output=True, text=True)
assert translated_images.stdout == native_images.stdout == "observed one matching ARM image, header and slide\n"
print("selected dyld count/name/header/slide match native ARM; unselected behavior preserved PASS")

# A genuinely library-validated source cannot admit these ad hoc libraries.
# Keep both original and carrier policy intact for the native and selected runs.
validated = main.parent / "validated-dynamic"
run(["clang", *arches, "-DREPORT_ENTRY", selection, "-o", validated])
run(["codesign", "-f", "-s", "-", "-o", "library", validated], capture_output=True)
native_refusal = subprocess.run([str(validated), str(branch)], capture_output=True, text=True, timeout=60)
assert (native_refusal.returncode == 3 and "validation-control-entered" in native_refusal.stderr
        and "not valid for use in process" in native_refusal.stderr and "-intel" not in native_refusal.stderr), native_refusal
(OUT / "native-library-validation.log").write_text(native_refusal.stderr)
run([ROOT / "tools/hao-embed", "--stub", HAO.parent / "hao-stub", "--host-test",
     "--arm-app-member", validated, "--in-place", validated], capture_output=True)
validated_refusal = selected(validated)
(OUT / "selected-library-validation.log").write_text(f"returncode={validated_refusal.returncode}\n" + validated_refusal.stderr)
if (platform.machine() == "arm64" and validated_refusal.returncode == -signal.SIGBUS and
        "validation-control-entered" not in validated_refusal.stderr):
    print("selected guest library-validation test SKIP: host x86 dyld terminated before guest entry")
else:
    assert (validated_refusal.returncode == 3 and "validation-control-entered" in validated_refusal.stderr
            and "dlopen refused:" in validated_refusal.stderr and
            ("not valid for use in process" in validated_refusal.stderr or "library validation" in validated_refusal.stderr)
            and "-intel" not in validated_refusal.stderr), validated_refusal

# Existing authored startup observer outside the selected app. The platform
# must suppress this optional instrumentation before it can run natively.
observer = OUT / "StartupObserver.dylib"
run(["clang", *arches, "-dynamiclib", "-DROLE_INSERT",
     ROOT / "tests/tools/identity_helper.c", "-o", observer])
sign(observer)
unmarked = OUT / "ordinary-x86"
run(["clang", "-arch", "x86_64", selection, "-DLINKED", branch, "-o", unmarked])
sign(unmarked)
control = subprocess.run(["/usr/bin/arch", "-x86_64", "-e", "DYLD_INSERT_LIBRARIES=" + str(observer),
                          str(unmarked)], capture_output=True, text=True, timeout=60, env=env)
assert control.returncode == 0 and "inserted library loaded" in control.stdout, control
result = selected(clients[0], ["-e", "DYLD_INSERT_LIBRARIES=" + str(observer)])
assert result.returncode == 0 and result.stdout == "result 11\n", result

saved_leaf = leaf.read_bytes()
arm_bad = bytearray(saved_leaf)
for i in range(int.from_bytes(arm_bad[4:8], "big")):
    cpu, _, offset, length, _ = struct.unpack_from(">IIIII", arm_bad, 8 + i * 20)
    if cpu == 0x100000c:
        marker = arm_bad.index(b"hao-domain-verification-leaf", offset, offset + length)
        arm_bad[marker] ^= 1
        break
else:
    raise AssertionError("missing arm64 fixture")
replacement = libs / "replacement"
replacement.write_bytes(arm_bad)
os.replace(replacement, leaf)
for client in clients:
    result = selected(client)
    assert result.returncode and "-intel" not in result.stderr and "result " not in result.stdout, result
leaf.write_bytes(saved_leaf)
run(["lipo", "-thin", "x86_64", leaf, "-output", replacement])
os.replace(replacement, leaf)
for client in clients:
    result = selected(client)
    assert result.returncode and "-intel" not in result.stderr and "result " not in result.stdout, result
print("selected linked, dlopen and dlsym use ARM through aliases; invalid/missing ARM refuse; restricted bootstrap suppresses prior native instrumentation PASS")
