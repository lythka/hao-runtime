#!/bin/bash
# The stub's launch image set and identity (hao-embed): every stub links the
# program's dependencies that load natively (the x86_64 shared cache, or a file
# with an x86_64 slice), weak ones weak, in the program's order, so the images
# present when libSystem initialises (libsecinit's app sandbox setup) are the
# native program's; and every stub has its own LC_UUID, derived from the
# program's.
#   OUT=dir [B=build] [EMBED=tools/hao-embed] tests/tools/stubdeps.sh
# Cases:
#  - app-sandboxed probe (tests/tools/sandbox.m) looking up launchservicesd:
#    Foundation-linked and plain C, native arm64 vs embedded under hao (the
#    sandbox allows it only when CoreFoundation was loaded at launch);
#  - the stub's LC_LOAD_DYLIB list is the program's natively loadable one:
#    weak kept weak, a fat @rpath library linked (with the program's rpaths;
#    its initializer runs once, as natively), an arm64-only library not linked;
#  - LC_UUIDs: not hao-stub's, different per program, the same in two runs.
# Runs sandboxed CLI programs on this host (container com.example.hao.stubdeps).
# Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools-stubdeps)}
B=${B:-$repo/build}
EMBED=${EMBED:-$repo/tools/hao-embed}
fails=0
# the x86_64 shared cache payload stubs (--stubs) are linked against: this host's
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT" || exit 2
embed() { python3 "$EMBED" --host-test --stub "$B/hao-stub" "$@"; }
uuid_of() { otool -arch "$1" -l "$2" | awk '/cmd LC_UUID/ {getline; getline; print $2; exit}'; }
deps_of() { otool -arch "$1" -L "$2" | tail -n +2 | sed 's/^[[:space:]]*//; s/ (compatibility version [^,]*, current version [^,)]*//; s/)$//'; }

# --- the app sandbox's launchservicesd lookup
cat >ents.plist <<'P'
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.app-sandbox</key><true/></dict></plist>
P
cat >Info.plist <<'P'
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>com.example.hao.stubdeps</string>
<key>CFBundleName</key><string>stubdeps</string></dict></plist>
P
for v in fnd c; do
    flags=(); [ $v = fnd ] && flags=(-DFOUNDATION -fobjc-arc -framework Foundation)
    for arch in arm64 arm64e; do
        clang -arch $arch -O1 ${flags[@]+"${flags[@]}"} -Wl,-sectcreate,__TEXT,__info_plist,Info.plist -o sbx-$v.$arch "$here/sandbox.m" &&
            codesign -s - -f -i com.example.hao.stubdeps --entitlements ents.plist sbx-$v.$arch 2>/dev/null || exit 2
    done
    embed -o sbx-$v.emb sbx-$v.arm64e >embed-$v.log || { bad "sandbox $v: hao-embed: $(cat embed-$v.log)"; continue; }
    timeout -s KILL 60 ./sbx-$v.arm64 >native-$v.out 2>&1
    timeout -s KILL 120 arch -x86_64 ./sbx-$v.emb >hao-$v.out 2>hao-$v.err
    n=$(grep -h "launchservicesd" native-$v.out) h=$(grep -h "launchservicesd" hao-$v.out)
    if [ -n "$n" ] && [ "$n" = "$h" ] && diff -q native-$v.out hao-$v.out >/dev/null; then
        pass "sandbox $v: native arm64 and embedded agree: $h"
    else bad "sandbox $v: native '$(tr '\n' ' ' <native-$v.out)', embedded '$(tr '\n' ' ' <hao-$v.out) $(tr '\n' ' ' <hao-$v.err)'"; fi
done
grep -q "launchservicesd kr=0" native-fnd.out && pass "sandbox fnd: native lookup allowed (kr=0)" ||
    bad "sandbox fnd: native lookup not allowed: $(cat native-fnd.out)"

# --- the stub's LC_LOAD_DYLIB list
mkdir -p lib
clang -arch arm64e -arch x86_64 -arch arm64 -dynamiclib -install_name @rpath/libfat.dylib -o lib/libfat.dylib "$here/stubdeps_lib.c" &&
    codesign -s - -f lib/libfat.dylib 2>/dev/null || exit 2
for arch in arm64 arm64e; do
    clang -arch $arch -dynamiclib -install_name @executable_path/libguest.dylib -o libguest.$arch.dylib "$here/stubdeps_guest.c" &&
        codesign -s - -f libguest.$arch.dylib 2>/dev/null || exit 2
done
cp libguest.arm64e.dylib libguest.dylib
clang -arch arm64e -O1 -o prog "$here/stubdeps_main.c" -framework CoreFoundation -weak_framework Security -Llib -lfat -L. -lguest \
    -Wl,-rpath,@executable_path/lib && codesign -s - -f prog 2>/dev/null || exit 2
embed -o prog.emb prog >embed-prog.log || bad "deps: hao-embed: $(cat embed-prog.log)"
deps_of x86_64 prog.emb | grep -v '^@rpath/libhao' >stub.deps
printf '%s\n' "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation" \
    "/System/Library/Frameworks/Security.framework/Versions/A/Security, weak" "@rpath/libfat.dylib" "/usr/lib/libSystem.B.dylib" >want.deps
if diff want.deps stub.deps >deps.diff; then
    pass "deps: stub links CoreFoundation, Security (weak), @rpath/libfat.dylib, libSystem; not the arm64-only libguest"
else bad "deps: stub's LC_LOAD_DYLIBs differ: $(tr '\n' ' ' <deps.diff)"; fi
otool -arch x86_64 -l prog.emb | grep -A2 LC_RPATH | grep -q "path @executable_path/lib " &&
    pass "deps: the program's LC_RPATH is carried for its @rpath library" || bad "deps: no @executable_path/lib rpath in the stub"
# the same program natively (arm64; libguest's arm64 slice)
clang -arch arm64 -O1 -o prog.arm64 "$here/stubdeps_main.c" -framework CoreFoundation -weak_framework Security -Llib -lfat \
    libguest.arm64.dylib -Wl,-rpath,@executable_path/lib && codesign -s - -f prog.arm64 2>/dev/null || exit 2
mkdir -p native && cp prog.arm64 native/prog && cp libguest.arm64.dylib native/libguest.dylib && cp -R lib native/
timeout -s KILL 60 native/prog >prog.native 2>&1
timeout -s KILL 120 arch -x86_64 ./prog.emb >prog.hao 2>prog.err
if [ -s prog.native ] && diff prog.native prog.hao >/dev/null; then
    pass "deps: embedded program runs as natively ($(cat prog.hao)): libfat's initializer ran once"
else bad "deps: native '$(cat prog.native)', embedded '$(cat prog.hao) $(head -3 prog.err)'"; fi

# --- LC_UUIDs
base=$(uuid_of x86_64 "$B/hao-stub")
u1=$(uuid_of x86_64 sbx-fnd.emb) u2=$(uuid_of x86_64 sbx-c.emb) u3=$(uuid_of x86_64 prog.emb)
embed -o sbx-fnd.emb2 sbx-fnd.arm64e >/dev/null
u1b=$(uuid_of x86_64 sbx-fnd.emb2)
if [ -n "$base" ] && [ -n "$u1" ] && [ "$(printf '%s\n' "$base" "$u1" "$u2" "$u3" | sort -u | wc -l)" -eq 4 ]; then
    pass "uuid: three stubs, three LC_UUIDs, none hao-stub's ($base)"
else bad "uuid: hao-stub $base, stubs $u1 $u2 $u3"; fi
[ -n "$u1" ] && [ "$u1" = "$u1b" ] && cmp -s sbx-fnd.emb sbx-fnd.emb2 && pass "uuid: the same program embedded twice: same LC_UUID $u1, same file" ||
    bad "uuid: two runs gave $u1 and $u1b"
[ "$u1" != "$(uuid_of arm64e sbx-fnd.emb)" ] && pass "uuid: the stub's LC_UUID is not the arm64e slice's" || bad "uuid: stub reuses the arm64e UUID"
# payload stubs (--stubs, no --host-test): two ObjC programs with nothing to
# carry were byte copies of hao-stub, one LC_UUID for both
mkdir -p root/usr/bin
for n in one two; do
    printf '#import <Foundation/Foundation.h>\nint main(void) { @autoreleasepool { NSLog(@"%s"); } return 0; }\n' $n >$n.m
    clang -arch arm64e -fobjc-arc -framework Foundation -o root/usr/bin/$n $n.m && codesign -s - -f root/usr/bin/$n 2>/dev/null || exit 2
done
python3 "$EMBED" --stub "$B/hao-stub" --stubs pay --root root --x86-cache "$X86" --scan >pay.log 2>&1 || bad "uuid: --stubs: $(cat pay.log)"
pu=$(for f in pay/stubs/*.stub; do uuid_of x86_64 "$f"; done | sort -u)
if [ "$(echo "$pu" | grep -c .)" = 2 ] && ! echo "$pu" | grep -qx "$base"; then
    pass "uuid: --stubs: two ObjC programs, two LC_UUIDs ($(echo $pu)), not hao-stub's"
else bad "uuid: --stubs: stub LC_UUIDs '$(echo $pu)' (hao-stub $base)"; fi
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); assert len(m["files"]) == 2 and all(e["links"] >= 2 and e["uuid"] for e in m["files"])' \
    pay/manifest.json && pass "uuid: --stubs: the manifest records each stub's UUID and link count" || bad "uuid: manifest fields"
echo "stubdeps: $fails failed"
exit $((fails != 0))
