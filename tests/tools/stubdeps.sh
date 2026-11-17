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
#    weak kept weak, an arm64-only library not linked, a fat @rpath library
#    linked only from protected storage (the sealed system volume: dyld runs
#    its initializer before hao sets CS_REQUIRE_LV), with the program's rpaths;
#    one in this temporary folder, under /Library or behind an earlier
#    rpath there is loaded by hao after library validation instead (its
#    initializer runs once, as natively);
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
    "/System/Library/Frameworks/Security.framework/Versions/A/Security, weak" "/usr/lib/libSystem.B.dylib" >want.deps
if diff want.deps stub.deps >deps.diff; then
    pass "deps: stub links CoreFoundation, Security (weak), libSystem; not the arm64-only libguest, not @rpath/libfat.dylib from this temporary folder"
else bad "deps: stub's LC_LOAD_DYLIBs differ: $(tr '\n' ' ' <deps.diff)"; fi
# the same library in protected storage (an image root's usr/bin) is linked; under /Library, or behind an
# earlier rpath under /Library, it is not (manifest not_linked says why)
mkdir -p sroot/usr/bin/lib sroot/Library/Tools/lib sroot/System/Library/CoreServices
# the image's OS version (what a library's minos is held to): this host's
printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>ProductVersion</key><string>%s</string></dict></plist>\n' \
    "$(sw_vers -productVersion)" >sroot/System/Library/CoreServices/SystemVersion.plist
cp lib/libfat.dylib sroot/usr/bin/lib/; cp lib/libfat.dylib sroot/Library/Tools/lib/
# these programs have the hardened runtime, as most of an image's do: dyld's restricted fallback paths
# (/usr/lib, /System/Library/Frameworks) are protected; prog11 (no hardened runtime) also falls back to /usr/local
cp prog sroot/usr/bin/prog; cp prog sroot/Library/Tools/prog; cp prog sroot/usr/bin/prog11
codesign -s - -f -o runtime sroot/usr/bin/prog sroot/Library/Tools/prog 2>/dev/null || exit 2
# rpath P: the program linked against @rpath/libfat.dylib with the rpath list P...
rprog() { local out=$1; shift
    clang -arch arm64e -O1 -o "$out" "$here/stubdeps_main.c" -framework CoreFoundation -weak_framework Security -Llib -lfat \
        -L. -lguest "$@" && codesign -s - -f -o runtime "$out" 2>/dev/null || exit 2; }
rprog sroot/usr/bin/prog3 -Wl,-rpath,/Library/Frameworks -Wl,-rpath,@executable_path/lib   # an earlier rpath under /Library
mkdir -p sroot/usr/local/lib; cp lib/libfat.dylib sroot/usr/local/lib/
rprog sroot/usr/bin/prog4 -Wl,-rpath,/usr/LOCAL/lib                                      # a firmlink in other case
ln -s ../usr/bin/lib sroot/Library/L
rprog sroot/usr/bin/prog5 -Wl,-rpath,/Library/L                                          # a link under /Library to usr/bin
python3 "$EMBED" --stub "$B/hao-stub" --stubs spay --root sroot --x86-cache "$X86" sroot/usr/bin/prog sroot/Library/Tools/prog \
    sroot/usr/bin/prog3 sroot/usr/bin/prog4 sroot/usr/bin/prog5 sroot/usr/bin/prog11 >spay.log 2>&1 || bad "deps: --stubs: $(cat spay.log)"
python3 -I - spay/manifest.json <<'PY' && pass "deps: @rpath/libfat.dylib linked from usr/bin/lib; not from Library/Tools/lib, behind a /Library rpath, through /usr/LOCAL or through a link under /Library to usr/bin (outside protected storage)" || bad "deps: protected storage: $(cat spay.log)"
import json, sys
m = {e["path"]: e for e in json.load(open(sys.argv[1]))["files"]}
why = lambda p: [w for n, w in m[p]["not_linked"] if n == "@rpath/libfat.dylib"]
assert why("usr/bin/prog") == [], m["usr/bin/prog"]["not_linked"]
for p, w in (("Library/Tools/prog", "/Library"), ("usr/bin/prog3", "/Library"), ("usr/bin/prog4", "/usr/local"),
             ("usr/bin/prog5", "/Library"), ("usr/bin/prog11", "/usr/local")):
    assert why(p) and why(p)[0].startswith("outside protected storage (%s)" % w), (p, why(p))
assert m["usr/bin/prog"]["links"] == m["Library/Tools/prog"]["links"] + 1
PY
# dyld goes past a path that exists but has no x86_64 library (here a text file in usr/bin/lib0) to the next,
# under /Library; and a library reached twice is judged in each rpath context and for each of its x86_64 slices
mkdir -p sroot/usr/bin/lib0 sroot/Library/L2 sroot/usr/bin/ctx sroot/usr/bin/uni ctx
echo 'not a library' >sroot/usr/bin/lib0/libfat.dylib; cp lib/libfat.dylib sroot/Library/L2/
rprog sroot/usr/bin/prog6 -Wl,-rpath,@executable_path/lib0 -Wl,-rpath,/Library/L2
echo 'void ctx_none(void) {}' >none.c
dl() { local out=$1 arch=$2 inst=$3; shift 3; clang -arch "$arch" -dynamiclib -install_name "$inst" -o "$out" none.c "$@" || exit 2; }
fat() { local out=$1; shift; lipo -create "$@" -output "$out" && codesign -s - -f "$out" 2>/dev/null || exit 2; }
# libD (protected, beside libC); libC (@loader_path/libC.dylib to both) needs @rpath/libD and has no rpath of its
# own: from libA the inherited rpaths find it beside libC,
# from libB /Library/X comes first (where a libD could be put)
for a in arm64e x86_64; do dl ctx/libD.$a $a @rpath/libD.dylib; done; fat sroot/usr/bin/ctx/libD.dylib ctx/libD.arm64e ctx/libD.x86_64
for a in arm64e x86_64; do dl ctx/libC.$a $a @loader_path/libC.dylib -Lsroot/usr/bin/ctx -lD; done; fat sroot/usr/bin/ctx/libC.dylib ctx/libC.arm64e ctx/libC.x86_64
for a in arm64e x86_64; do dl ctx/libA.$a $a @rpath/libA.dylib -Lsroot/usr/bin/ctx -lC -Wl,-rpath,@loader_path; done
fat sroot/usr/bin/ctx/libA.dylib ctx/libA.arm64e ctx/libA.x86_64
for a in arm64e x86_64; do dl ctx/libB.$a $a @rpath/libB.dylib -Lsroot/usr/bin/ctx -lC -Wl,-rpath,/Library/X -Wl,-rpath,@loader_path; done
fat sroot/usr/bin/ctx/libB.dylib ctx/libB.arm64e ctx/libB.x86_64
printf 'int main(void) { return 0; }\n' >m.c
# libL needs both, so libC is reached twice in one closure
for a in arm64e x86_64; do dl ctx/libL.$a $a @rpath/libL.dylib -Lsroot/usr/bin/ctx -lA -lB -Wl,-rpath,@loader_path; done
fat sroot/usr/bin/ctx/libL.dylib ctx/libL.arm64e ctx/libL.x86_64
clang -arch arm64e -o sroot/usr/bin/prog7 m.c -Lsroot/usr/bin/ctx -lL -Wl,-rpath,@executable_path/ctx && codesign -s - -f -o runtime sroot/usr/bin/prog7 2>/dev/null || exit 2
# libU: x86_64 slice with rpath @loader_path, x86_64h slice with /Library/X first, both needing @rpath/libD
for a in arm64e x86_64 x86_64h; do
    r=@loader_path/../ctx; [ $a = x86_64h ] && r=/Library/X
    dl ctx/libU.$a $a @rpath/libU.dylib -Lsroot/usr/bin/ctx -lD -Wl,-rpath,$r -Wl,-rpath,@loader_path/../ctx
done
fat sroot/usr/bin/uni/libU.dylib ctx/libU.arm64e ctx/libU.x86_64 ctx/libU.x86_64h
clang -arch arm64e -o sroot/usr/bin/prog8 m.c -Lsroot/usr/bin/uni -lU -Wl,-rpath,@executable_path/uni && codesign -s - -f -o runtime sroot/usr/bin/prog8 2>/dev/null || exit 2
# a library dyld might not load into the stub ends nothing either: x86_64h only (usr/bin/libh), or built for
# a newer macOS than the image's (usr/bin/libn); the next path is under /Library
mkdir -p sroot/usr/bin/libh sroot/usr/bin/libn
clang -arch x86_64h -dynamiclib -install_name @rpath/libfat.dylib -o sroot/usr/bin/libh/libfat.dylib "$here/stubdeps_lib.c" \
    && codesign -s - -f sroot/usr/bin/libh/libfat.dylib 2>/dev/null || exit 2
clang -arch x86_64 -dynamiclib -install_name @rpath/libfat.dylib -o sroot/usr/bin/libn/libfat.dylib "$here/stubdeps_lib.c" || exit 2
python3 -I - sroot/usr/bin/libn/libfat.dylib <<'PY' && codesign -s - -f sroot/usr/bin/libn/libfat.dylib 2>/dev/null || exit 2
import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
at = 32
for _ in range(struct.unpack_from("<I", b, 16)[0]):
    cmd, size = struct.unpack_from("<II", b, at)
    if cmd == 0x32:
        struct.pack_into("<I", b, at + 12, 99 << 16)   # minos 99.0
    at += size
open(sys.argv[1], "wb").write(b)
PY
rprog sroot/usr/bin/prog9 -Wl,-rpath,@executable_path/libh -Wl,-rpath,/Library/L2
rprog sroot/usr/bin/prog10 -Wl,-rpath,@executable_path/libn -Wl,-rpath,/Library/L2
python3 "$EMBED" --stub "$B/hao-stub" --stubs spay2 --root sroot --x86-cache "$X86" sroot/usr/bin/prog6 sroot/usr/bin/prog7 \
    sroot/usr/bin/prog8 sroot/usr/bin/prog9 sroot/usr/bin/prog10 >spay2.log 2>&1 || bad "deps: --stubs (search): $(cat spay2.log)"
python3 -I - spay2/manifest.json <<'PY' && pass "deps: a path with no x86_64 library, an x86_64h-only one or one for a newer macOS does not end dyld's search; a library is judged in every rpath context and for every x86_64 slice" || bad "deps: search and contexts: $(cat spay2.log)"
import json, sys
m = {e["path"]: dict(e["not_linked"]) for e in json.load(open(sys.argv[1]))["files"]}
assert m["usr/bin/prog6"]["@rpath/libfat.dylib"].startswith("outside protected storage (/Library)"), m["usr/bin/prog6"]
assert m["usr/bin/prog7"]["@rpath/libL.dylib"].startswith("outside protected storage (/Library)"), m["usr/bin/prog7"]
assert m["usr/bin/prog8"]["@rpath/libU.dylib"].startswith("outside protected storage (/Library)"), m["usr/bin/prog8"]
for p in ("usr/bin/prog9", "usr/bin/prog10"):
    assert m[p]["@rpath/libfat.dylib"].startswith("outside protected storage (/Library)"), (p, m[p])
PY
# the identity gate: these stubs pass; usr/bin/prog's stub (it links @rpath/libfat.dylib) checked as the stub of
# the same program under /Library fails dylibs.protected
python3 -I - "$repo/tools" spay sroot "$X86" <<'PY' && pass "deps: the identity gate passes the stubs and finds a linked library outside protected storage" || bad "deps: identity gate dylibs.protected"
import json, os, sys
sys.path.insert(0, sys.argv[1])
import stub_identity_gate as g
embed = g.load_embed()
x86 = embed.X86Libraries(sys.argv[4], sys.argv[3])
m = {e["path"]: e for e in json.load(open(os.path.join(sys.argv[2], "manifest.json")))["files"]}
def rows(path, stubpath):
    src = os.path.join(sys.argv[3], path)
    arm = embed.arm_slice(open(src, "rb").read())
    stub = open(os.path.join(sys.argv[2], m[stubpath]["stub"]), "rb").read()
    return {f: v for f, v, *_ in g.compare(arm, stub, embed, src, x86)}
for p in m:
    assert rows(p, p)["dylibs.protected"] == "SAME", p
assert rows("Library/Tools/prog", "usr/bin/prog")["dylibs.protected"] == "DIFF"
PY
otool -arch x86_64 -l prog.emb | grep -A2 LC_RPATH | grep -q "path @executable_path/lib " &&
    pass "deps: the program's LC_RPATH is carried (hao's binder and native dlopens expand @rpath with it)" || bad "deps: no @executable_path/lib rpath in the stub"
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
