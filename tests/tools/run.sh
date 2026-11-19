#!/bin/bash
# Image tools: hao-embed --in-place with hard links and symlinks, hao-install.py
# run twice (idempotent) and its path rules (tests/tools/test_install.py),
# kext/stamp_identity.py's layout checks, and
# tests/tools/bundle.sh (bundle main executables: Info.plist and resource seal bound) and
# tests/tools/stubdeps.sh (the stub links the program's native dependencies; per-stub LC_UUIDs),
# tests/tools/platform.sh (the stub's LC_BUILD_VERSION is the program's: Mac Catalyst, macOS),
# tests/tools/identity.sh (the stub's load-time identity by rule, tools/stub_identity_gate.py),
# tests/tools/aotpayload.sh (AOT caches shipped in the payload, installed and trusted; stub-identities),
# tools/hao-runtime-on-mac (the archive's on-Mac files; tests/tools/onmac_pin_check.py),
# tests/tools/codeid.sh (the code identity Security.framework gives listed stubs, src/code_identity.c) and
# tests/tools/test_codesig.py (tools/hao_codesig.py and src/libval.c read a signature alike) and
# tests/tools/test_trustlist.py (hao-trust-list hashes and verifies one copy of a file) and
# tests/tools/test_root.py (--cryptex: a cryptex beside ROOT, the same outputs as in place).
#   OUT=dir [B=build] [KC=BootKernelExtensions.kc] tests/tools/run.sh
# Supply KC explicitly for read-only kernel collection checks; stamp cases
# are skipped without it. Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-pkgtest)}
B=${B:-$repo/build}
KC=${KC:-}
fails=0
# the x86_64 shared cache payload stubs (--stubs) are linked against: this host's
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
# payman PAYLOAD: its payload-manifest.json as tools/hao-image-payload writes it (hao-install.py installs only what
# that lists)
payman() {
    python3 -I -c 'import hashlib, json, os, sys
p = sys.argv[1]
f = {os.path.relpath(os.path.join(b, n), p): hashlib.sha256(open(os.path.join(b, n), "rb").read()).hexdigest()
     for b, _, ns in os.walk(p) for n in ns if n != "payload-manifest.json" or b != p}
json.dump({"files": f}, open(os.path.join(p, "payload-manifest.json"), "w"))' "$1"
}
mkdir -p "$OUT"
G=$OUT/prog
clang -arch arm64e -O1 -o "$G" "$repo/tests/guest/hello.c" && codesign -s - -f "$G" 2>/dev/null || exit 2

# --- hao-embed --in-place keeps the inode (hard links) and refuses symlinks
d=$OUT/links; mkdir -p "$d"
cp "$G" "$d/a"; ln -f "$d/a" "$d/b"; ln -sf a "$d/c"
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" --in-place "$d/c" "$d/a" >"$d/log" 2>&1
grep -q "^fail.*c.*symlink" "$d/log" && pass "in-place: symlink refused" || bad "in-place symlink: $(cat "$d/log")"
[ -L "$d/c" ] && pass "in-place: symlink left a symlink" || bad "in-place: symlink replaced by a file"
if lipo -archs "$d/b" | grep -q x86_64 && [ "$(stat -f %i "$d/a")" = "$(stat -f %i "$d/b")" ]; then
    pass "in-place: the other hard link is embedded too (same inode)"
else bad "in-place hard link: b is $(lipo -archs "$d/b"), inodes $(stat -f %i "$d/a") $(stat -f %i "$d/b")"; fi
# -o writes a new file and renames it over the output: a symlink there (or at the old fixed temporary
# name OUT.hao-embed) is replaced or left alone, never written through
echo victim >"$d/victim"; cp "$d/victim" "$d/victim2"; ln -sf victim "$d/out"; ln -sf victim2 "$d/out.hao-embed"
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$d/out" "$G" >"$d/out.log" 2>&1
if [ "$(cat "$d/victim" "$d/victim2")" = "$(printf 'victim\nvictim')" ] && [ ! -L "$d/out" ] && lipo -archs "$d/out" | grep -q x86_64 \
    && [ "$(ls -a "$d" | grep -c '^\.hao-embed')" = 0 ]; then
    pass "-o: a symlink at the output is replaced, not written through; no fixed temporary name is used"
else bad "-o through a symlink: $(cat "$d/out.log"; cat "$d/victim" "$d/victim2"; ls -la "$d")"; fi
# --- launch constraints (signature slots 8-11) go into the stub's signature byte for byte; one codesign cannot
# express (a category constraint) refuses the program unless --allow-dropped-constraints pins that blob
d=$OUT/lc; rm -rf "$d"; mkdir -p "$d"
printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>is-init-proc</key><true/></dict></plist>\n' >"$d/parent.plist"
cp "$G" "$d/carried"; codesign -s - -f --launch-constraint-parent "$d/parent.plist" "$d/carried" 2>/dev/null
python3 -I - "$repo/tools" "$d/carried" "$d/category" <<'PY' || bad "launch constraints: fixture"
import sys
sys.path.insert(0, sys.argv[1])
import hao_codesig
mo = open(sys.argv[2], 'rb').read()
blob = hao_codesig.constraints(mo)[9]
assert hao_codesig.constraint(blob) == {'ccat': 0, 'comp': 1, 'reqs': {'is-init-proc': True}, 'vers': 1}
cat = blob.replace(bytes.fromhex('0c046363617402010030'), bytes.fromhex('0c046363617402010130'), 1)   # ccat 0 -> 1
assert cat != blob and hao_codesig.constraint(cat)['ccat'] == 1
out = bytearray(mo); out[out.index(blob):out.index(blob) + len(blob)] = cat
open(sys.argv[3], 'wb').write(hao_codesig.bind_special_slots(bytes(out), {9: cat}))
PY
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$d/carried.emb" "$d/carried" >"$d/a.log" 2>&1
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$d/category.emb" "$d/category" >"$d/b.log" 2>&1; rb=$?
# pins: PATH parent CDHASH SHA256 for FILE (the arm slice's cdhash, the parent constraint blob's sha256)
pin() { python3 -I -c 'import hashlib, sys
sys.path.insert(0, sys.argv[1])
import hao_codesig, hao_fat
sl = {c: p for c, _, p in hao_fat.slices(open(sys.argv[3], "rb").read())}
b = hao_codesig.constraints(sl[hao_fat.CPU_ARM64])[9]
print(sys.argv[2], "parent", hao_codesig.check(sl[hao_fat.CPU_ARM64])["cdhash"].hex(), sys.argv[4] if len(sys.argv) > 4 else hashlib.sha256(b).hexdigest())' "$repo/tools" "$@"; }
pin "$d/category" "$d/category" "$(printf '0%.0s' $(seq 64))" >"$d/allow.wrong"
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" --allow-dropped-constraints "$d/allow.wrong" -o "$d/wrong.emb" "$d/category" >"$d/w.log" 2>&1; rw=$?
pin "$d/category" "$d/category" >"$d/allow"
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" --allow-dropped-constraints "$d/allow" -o "$d/dropped.emb" "$d/category" >"$d/c.log" 2>&1; rc=$?
pin "$d/dropped.emb" "$d/category" >"$d/gate.allow"
python3 -I "$repo/tools/stub_identity_gate.py" --host-test "$d/carried.emb" >"$d/gate.log" 2>&1; rg=$?
python3 -I "$repo/tools/stub_identity_gate.py" --host-test "$d/dropped.emb" >"$d/gate2.log" 2>&1; rg2=$?
python3 -I "$repo/tools/stub_identity_gate.py" --host-test --dropped-constraints "$d/gate.allow" "$d/dropped.emb" >"$d/gate3.log" 2>&1; rg3=$?
if python3 -I - "$repo/tools" "$d/carried.emb" "$d/dropped.emb" <<'PY' && [ $rb = 1 ] && grep -q "parent constraint codesign cannot carry" "$d/b.log" && [ $rc = 0 ] \
    && [ $rw = 1 ] && grep -q "does not name that blob" "$d/w.log" \
    && [ $rg = 0 ] && [ $rg2 = 1 ] && grep -q "sig.constraint-parent.*DIFF\|DIFF.*sig.constraint-parent" "$d/gate2.log" && [ $rg3 = 0 ]; then
import sys
sys.path.insert(0, sys.argv[1])
import hao_codesig, hao_fat
for f, want in ((sys.argv[2], True), (sys.argv[3], False)):
    sl = {c: p for c, _, p in hao_fat.slices(open(f, 'rb').read())}
    arm, x86 = hao_codesig.constraints(sl[hao_fat.CPU_ARM64]), hao_codesig.constraints(sl[hao_fat.CPU_X86_64])
    assert set(arm) == {9} and (x86 == arm) == want and (x86 == {}) != want, (f, arm, x86)
PY
    pass "launch constraints: a parent constraint is sealed into the stub byte for byte; a category constraint refuses the program unless its blob is pinned (another digest is refused), then is left out; the gate: SAME, DIFF unless the release list pins it"
else bad "launch constraints: rb $rb rc $rc rw $rw gate $rg $rg2 $rg3: $(cat "$d/a.log" "$d/b.log" "$d/c.log" "$d/w.log"; grep constraint "$d/gate.log" "$d/gate2.log" "$d/gate3.log")"; fi
# --- a program that requires library validation (library flag) gets a hardened-runtime stub: dyld ignores
# DYLD_INSERT_LIBRARIES for it as natively, and hao still runs it; a plain program's stub does not
d=$OUT/lvrt; rm -rf "$d"; mkdir -p "$d"
cp "$G" "$d/lv"; codesign -s - -f -o library "$d/lv" 2>/dev/null; cp "$G" "$d/plain"
printf '#include <stdio.h>\n__attribute__((constructor)) static void i(void) { fprintf(stderr, "INSERTED\\n"); }\n' >"$d/ins.c"
printf '#include <unistd.h>\nint main(int c, char **v, char **e) { execve(v[1], v + 1, e); return 127; }\n' >"$d/x.c"
clang -arch x86_64 -dynamiclib -o "$d/ins.dylib" "$d/ins.c" && clang -arch x86_64 -o "$d/x" "$d/x.c" && codesign -s - -f "$d/ins.dylib" "$d/x" 2>/dev/null || exit 2
for f in lv plain; do "$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$d/$f.emb" "$d/$f" >"$d/$f.log" 2>&1; done
# x (x86_64, unrestricted) keeps the variable and execs the stub; timeout and env are platform programs, which
# dyld strips DYLD_* from, so env sets it after them
nl=$(cd "$d" && timeout -s KILL 60 env DYLD_INSERT_LIBRARIES="$d/ins.dylib" ./x ./lv.emb 2>&1); rl=$?
np=$(cd "$d" && timeout -s KILL 60 env DYLD_INSERT_LIBRARIES="$d/ins.dylib" ./x ./plain.emb 2>&1); rp=$?
python3 -I "$repo/tools/stub_identity_gate.py" --host-test "$d/lv.emb" "$d/plain.emb" >"$d/gate.log" 2>&1; rg=$?
if [ $rl = 7 ] && [ $rp = 7 ] && [ "$(echo "$nl" | grep -c INSERTED)" = 1 ] && [ "$(echo "$np" | grep -c INSERTED)" = 2 ] \
    && codesign -dv --arch x86_64 "$d/lv.emb" 2>&1 | grep -q "flags=.*runtime" && [ $rg = 0 ]; then
    pass "library flag: the stub has the hardened runtime, dyld inserts nothing into it, hao runs it (rc 7); a plain program's stub takes the insert; gate SAME"
else bad "library flag: rc $rl $rp gate $rg: [$nl] [$np] $(grep DIFF "$d/gate.log")"; fi
# --- a sandbox the kernel applies at exec (com.apple.private.sandbox.profile) may deny the process setting
# CS_REQUIRE_LV on itself: the stub of such a program that requires library validation has the library flag in
# its signature (the kernel sets it at exec); without the entitlement, or without library validation, it has
# not. Static: the private entitlement is kept (no --host-test), nothing is launched
d=$OUT/xsb; rm -rf "$d"; mkdir -p "$d"
printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>com.apple.private.sandbox.profile:macos</key><string>com.example.profile</string></dict></plist>\n' >"$d/sb.plist"
cp "$G" "$d/sbrt"; cp "$G" "$d/rt"; cp "$G" "$d/sb"
codesign -s - -f -o runtime --entitlements "$d/sb.plist" "$d/sbrt" 2>/dev/null; codesign -s - -f -o runtime "$d/rt" 2>/dev/null
codesign -s - -f --entitlements "$d/sb.plist" "$d/sb" 2>/dev/null
xf=""
for f in sbrt rt sb; do
    "$repo/tools/hao-embed" --stub "$B/hao-stub" -o "$d/$f.emb" "$d/$f" >"$d/$f.log" 2>&1 || xf="$xf $f:embed"
    codesign -dv --arch x86_64 "$d/$f.emb" 2>&1 | grep -q "flags=.*library-validation" && xf="$xf $f:lv" || xf="$xf $f:-"
done
[ "$xf" = " sbrt:lv rt:- sb:-" ] && pass "sandbox at exec: a library-validated program's stub has the library flag; others not" ||
    bad "sandbox at exec: library flag per stub:$xf"
d=$OUT/links
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/x" --scan >/dev/null 2>"$d/scan.err"
grep -q "needs --root" "$d/scan.err" && pass "--scan without --root: usage error" || bad "--scan without --root: $(cat "$d/scan.err")"

# --- every stub names the ARM slice it was made for (src/libval.c loads no other under it)
python3 -I - "$repo/tools" "$d/a" <<'PY' && pass "stub names its ARM slice: __TEXT,__hao_slice holds the slice's hash type and cdhash" || bad "stub's __hao_slice record"
import struct, sys
sys.path.insert(0, sys.argv[1])
import hao_codesig, hao_fat
parts = hao_fat.slices(open(sys.argv[2], "rb").read())
arm = [bytes(p) for c, s, p in parts if c == hao_fat.CPU_ARM64][0]
stub = [bytes(p) for c, s, p in parts if c == hao_fat.CPU_X86_64][0]
have = None
for cmd, off, size in hao_fat.load_commands(stub):
    if cmd == 0x19:
        for k in range(struct.unpack_from("<I", stub, off + 64)[0]):
            so = off + 72 + 80 * k
            if stub[so:so + 16].rstrip(b"\0") == b"__hao_slice" and stub[so + 16:so + 32].rstrip(b"\0") == b"__TEXT":
                ssize, foff = struct.unpack_from("<QI", stub, so + 40)
                have = stub[foff:foff + ssize]
c = hao_codesig.check(arm)
assert c["verdict"] == "valid" and have == b"haoslice" + bytes([1, c["hash_type"], 0, 0]) + c["cdhash"], (have, c["verdict"])
PY
cp "$G" "$OUT/unsigned" && codesign --remove-signature "$OUT/unsigned"
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$OUT/unsigned.emb" "$OUT/unsigned" >"$OUT/unsigned.log" 2>&1
grep -q "^fail.*not code signed" "$OUT/unsigned.log" && [ ! -e "$OUT/unsigned.emb" ] && pass "hao-embed refuses a program whose ARM slice is not signed" ||
    bad "hao-embed with an unsigned program: $(cat "$OUT/unsigned.log")"
cp "$G" "$OUT/badpage" && printf 'X' | dd of="$OUT/badpage" bs=1 seek=16390 conv=notrunc 2>/dev/null
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$OUT/badpage.emb" "$OUT/badpage" >"$OUT/badpage.log" 2>&1
grep -q "^fail.*code signature is not valid" "$OUT/badpage.log" && [ ! -e "$OUT/badpage.emb" ] && pass "hao-embed refuses a program whose ARM slice does not match its page hashes" ||
    bad "hao-embed with a changed program: $(cat "$OUT/badpage.log")"

# --- hao-install.py twice on the same System
d=$OUT/install; mkdir -p "$d/src/usr/bin" "$d/sys/usr/bin" "$d/sys/usr/lib/hao" "$d/pay/root/usr/lib/hao"
cp "$G" "$d/src/usr/bin/prog"; cp "$G" "$d/sys/usr/bin/prog"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/pay/embed" --root "$d/src" --x86-cache "$X86" --require usr/bin/prog "$d/src/usr/bin/prog" >/dev/null
payman "$d/pay"
python3 -I "$repo/tools/hao-install.py" "$d/sys" "$d/pay" >"$d/1.log" 2>&1; r1=$?
python3 -I "$repo/tools/hao-install.py" "$d/sys" "$d/pay" >"$d/2.log" 2>&1; r2=$?
if [ $r1 = 0 ] && [ $r2 = 0 ] && grep -q '"already_embedded": 1' "$d/sys/usr/lib/hao/install.json"; then
    pass "hao-install.py twice: rc $r1, $r2; second run counts the file as already embedded"
else bad "hao-install.py twice: rc $r1 $r2: $(cat "$d/2.log")"; fi
# --- hao-install.py keeps manifest names under SYSTEM and PAYLOAD and follows no symlink
if python3 -I "$here/test_install.py" >"$d/paths.log" 2>&1; then
    pass "hao-install.py path rules: $(grep '^Ran' "$d/paths.log") (unsafe manifest names and symlinks refused)"
else bad "hao-install.py path rules: $(tail -15 "$d/paths.log")"; fi
# --- tools/hao_ifunit.py match: each interface unit names an image of the x86_64 cache (hao-image-payload --units)
if python3 -I "$here/test_ifunit_match.py" >"$d/ifmatch.log" 2>&1; then
    pass "interface units against the x86_64 cache: $(grep '^Ran' "$d/ifmatch.log") (another library's or another name's unit refused)"
else bad "hao_ifunit.py match: $(tail -15 "$d/ifmatch.log")"; fi
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); assert m["inputs_sha256"] and all("stub_sha256" in e for e in m["files"])' \
    "$d/pay/embed/manifest.json" && pass "manifest records per-stub hashes and build inputs" || bad "manifest hashes"
# --- stub skeletons: filled from the program without ld, the stub equals the linked one, and the
# skeleton holds none of the program's carried bytes
d=$OUT/skeleton; rm -rf "$d"; mkdir -p "$d/src/usr/bin" "$d/noclang"
cp "$G" "$d/src/usr/bin/prog"
printf '#!/bin/sh\necho "$0 called" >&2\nexit 99\n' > "$d/noclang/clang"; chmod +x "$d/noclang/clang"; cp "$d/noclang/clang" "$d/noclang/ld"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/a" --root "$d/src" --x86-cache "$X86" --save-skeletons "$d/skel" \
    "$d/src/usr/bin/prog" >/dev/null 2>"$d/a.err"; ra=$?
PATH="$d/noclang:$PATH" "$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/b" --root "$d/src" --x86-cache "$X86" \
    --skeletons "$d/skel" "$d/src/usr/bin/prog" >/dev/null 2>"$d/b.err"; rb=$?
if [ $ra = 0 ] && [ $rb = 0 ] && ! grep -q called "$d/b.err" && diff -r "$d/a/stubs" "$d/b/stubs" >/dev/null &&
   python3 -c 'import json,glob,sys
for j in glob.glob(sys.argv[1] + "/*.json"):
    b = open(j[:-5] + ".skel", "rb").read()
    assert all(not any(b[o:o + n]) for _, _, o, n in json.load(open(j))["sections"])' "$d/skel"; then
    pass "stub skeleton: filled without ld, the same stub as linked; carried sections zeroed"
else bad "stub skeleton: rc $ra $rb: $(cat "$d/a.err" "$d/b.err" | tail -3)"; fi

# --- tools/hao-runtime-on-mac: from an archive's layout (build/, tools/, payload/ with skeletons and manifests) and
# ROOT, the stubs and the dyld fix come out with the manifest's hashes; a missing AOT cache is allowed, a wrong
# hash is not. ROOT's dyld is this host's (the generator needs a 27.x dyld).
d=$OUT/onmac; rm -rf "$d"; mkdir -p "$d/src/sbin" "$d/src/usr/libexec" "$d/src/usr/lib" "$d/arch/build" "$d/arch/tools" "$d/arch/payload/embed"
cp "$G" "$d/src/sbin/launchd"; cp "$G" "$d/src/usr/libexec/xpcproxy"; cp /usr/lib/dyld "$d/src/usr/lib/dyld"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/orig" --root "$d/src" --x86-cache "$X86" \
    --save-skeletons "$d/arch/payload/embed/skeletons" --require sbin/launchd --require usr/libexec/xpcproxy \
    "$d/src/sbin/launchd" "$d/src/usr/libexec/xpcproxy" >/dev/null 2>"$d/orig.err"
python3 -I "$repo/tools/dyld-ignition-fix.py" "$d/src/usr/lib/dyld" "$d/dyld.want" >/dev/null 2>&1
mkdir -p "$d/arch/build/front"; for f in hao-stub front/stub.o libhao.dylib libhao_identity.dylib; do cp "$B/$f" "$d/arch/build/$f"; done
cp "$d/orig/manifest.json" "$d/arch/payload/embed/"
for t in hao-embed dyld-ignition-fix.py hao-runtime-on-mac hao_fat.py hao_codesig.py hao_macho.py hao_root.py stub_identity_gate.py hao.entitlements; do
    cp "$repo/tools/$t" "$d/arch/tools/"
done
python3 -I - "$d" <<'PY'
import hashlib, json, os, sys
d = sys.argv[1]
h = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
files = {'embed/stubs/' + n: h(os.path.join(d, 'orig/stubs', n)) for n in os.listdir(os.path.join(d, 'orig/stubs'))}
files['replace/dyld'] = h(os.path.join(d, 'dyld.want'))
files['root/usr/lib/hao/aot/00000000000000000000000000000000.aot'] = '0' * 64
pay = os.path.join(d, 'arch/payload')
for b, _, ns in os.walk(pay):
    for n in ns:
        files[os.path.relpath(os.path.join(b, n), pay)] = h(os.path.join(b, n))
json.dump({'files': files}, open(os.path.join(pay, 'payload-manifest.json'), 'w'))
PY
mkdir -p "$d/arch/payload/replace"; ln -s "$d/victim" "$d/arch/payload/replace/dyld"   # an output path is replaced, never followed
python3 -I "$d/arch/tools/hao-runtime-on-mac" "$d/src" "$X86" >"$d/run.log" 2>&1; r1=$?
[ ! -e "$d/victim" ] && [ ! -L "$d/arch/payload/replace/dyld" ] && pass "hao-runtime-on-mac: a symbolic link at an output path is replaced, not written through" \
    || bad "hao-runtime-on-mac: wrote through the link at replace/dyld"
echo '<plist/>' > "$d/arch/payload/extra.plist"
python3 -I "$d/arch/tools/hao-runtime-on-mac" "$d/src" "$X86" >"$d/extra.log" 2>&1; r4=$?
rm "$d/arch/payload/extra.plist"
[ $r4 = 1 ] && grep -q 'extra.plist: not in payload-manifest.json' "$d/extra.log" \
    && pass "hao-runtime-on-mac: a payload file the manifest does not list fails the check" || bad "hao-runtime-on-mac: unlisted file rc $r4: $(tail -2 "$d/extra.log")"
python3 -I -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m["files"]["replace/dyld"]="1"*64; json.dump(m,open(p,"w"))' \
    "$d/arch/payload/payload-manifest.json"
python3 -I "$d/arch/tools/hao-runtime-on-mac" "$d/src" "$X86" "$d/arch" >"$d/bad.log" 2>&1; r2=$?
python3 -I "$here/onmac_pin_check.py" "$d"/orig/stubs/* "$d/dyld.want" >"$d/pin.log" 2>&1; r3=$?
if [ $r1 = 0 ] && grep -q '[0-9]* files match payload-manifest.json, 0 differ' "$d/run.log" && [ $r2 = 1 ] && grep -q 'replace/dyld: sha256 differs' "$d/bad.log" \
    && [ $r3 = 0 ]; then
    pass "hao-runtime-on-mac: stubs and dyld fix from the archive layout match the manifest; a wrong hash fails; a larger signature reservation (macOS 26 codesign) is trimmed to the pinned bytes"
else bad "hao-runtime-on-mac: rc $r1 $r2 $r3: $(tail -3 "$d/run.log" "$d/bad.log" "$d/pin.log")"; fi

# --- a hard-linked program (27.0 usr/libexec and usr/sbin/appleh13camerad): --scan lists
# both paths, and the installer embeds both whether the image keeps the link or not
d=$OUT/hardlink; rm -rf "$d"; mkdir -p "$d/src/usr/libexec" "$d/src/usr/sbin" "$d/pay/root/usr/lib/hao"
cp "$G" "$d/src/usr/libexec/prog"; ln -f "$d/src/usr/libexec/prog" "$d/src/usr/sbin/prog"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/pay/embed" --root "$d/src" --x86-cache "$X86" --scan >/dev/null
payman "$d/pay"
for kind in linked unlinked; do
    s=$d/sys-$kind; mkdir -p "$s/usr/libexec" "$s/usr/sbin" "$s/usr/lib/hao"
    cp "$G" "$s/usr/libexec/prog"
    if [ $kind = linked ]; then ln -f "$s/usr/libexec/prog" "$s/usr/sbin/prog"; else cp "$G" "$s/usr/sbin/prog"; fi
    python3 -I "$repo/tools/hao-install.py" "$s" "$d/pay" >"$d/$kind.log" 2>&1; r=$?
    if [ $r = 0 ] && lipo -archs "$s/usr/libexec/prog" | grep -q x86_64 && lipo -archs "$s/usr/sbin/prog" | grep -q x86_64; then
        pass "hard-linked program, image link $kind: both paths embedded"
    else bad "hard-linked program, image link $kind: rc $r, sbin $(lipo -archs "$s/usr/sbin/prog"): $(cat "$d/$kind.log")"; fi
done

# --- stamp_identity.py
if [ -f "$KC" ]; then
    d=$OUT/stamp; mkdir -p "$d"
    "$repo/kext/build.sh" "$d" >/dev/null 2>&1 || bad "kext build"
    if python3 -I "$repo/kext/stamp_identity.py" "$KC" "$d/HaoExec.kext" >"$d/log" 2>&1; then
        python3 - "$d/HaoExec.kext/Contents/MacOS/HaoExec" <<'PY' && pass "stamp v2: layout [mach, fat, shell, NULL] and three slide checks stamped" || bad "stamp contents"
import struct, sys
d = open(sys.argv[1], "rb").read()
i = d.find(struct.pack("<II", 0x494F4148, 2))
v = struct.unpack_from("<II16sQQQQQQQQQ", d, i)
assert i >= 0 and all(v[3:]), v
PY
    else bad "stamp: $(cat "$d/log")"; fi
    python3 -I - "$repo/kext" "$KC" <<'PY' && pass "stamp refuses an activator table without its NULL terminator" || bad "stamp accepted a 4-entry table"
import sys
sys.path.insert(0, sys.argv[1])
import stamp_identity as s
import struct
kc = bytearray(open(sys.argv[2], "rb").read())
uuid, hdr, pf, table, acts, checks = s.kernel_identity(bytes(kc))
# the table's bytes in the file (three activators, then NULL): make it a fourth entry
pat = struct.pack("<4Q", *acts, 0)
i = kc.find(pat)
assert i >= 0
kc[i + 24:i + 32] = struct.pack("<Q", acts[0])
try:
    s.kernel_identity(bytes(kc))
except SystemExit as e:
    sys.exit(0 if "NULL" in str(e) else 1)
sys.exit(1)
PY
elif [ -z "$KC" ]; then echo "SKIP stamp cases (KC not set: a BootKernelExtensions.kc of the target kernel)"
else echo "SKIP stamp cases (KC=$KC is not a file)"; fi
# --- bundle main executables: the stub binds the bundle's Info.plist and resource seal
OUT=$OUT/bundle B=$B "$here/bundle.sh" || bad "tests/tools/bundle.sh"
# --- the stub's launch image set (the program's native dependencies) and its own LC_UUID
OUT=$OUT/stubdeps B=$B X86_CACHE=$X86 "$here/stubdeps.sh" || bad "tests/tools/stubdeps.sh"
# --- the stub's LC_BUILD_VERSION is the program's (Mac Catalyst and macOS)
OUT=$OUT/platform B=$B X86_CACHE=$X86 "$here/platform.sh" || bad "tests/tools/platform.sh"
# --- the stub's load-time identity by rule (source version, rpaths, environment, stack size, CS flags, gate)
OUT=$OUT/identity B=$B X86_CACHE=$X86 "$here/identity.sh" || bad "tests/tools/identity.sh"
# --- AOT caches in the payload (hao-image-payload --aot) and as hao-install.py installs them
OUT=$OUT/aotpayload X86_CACHE=$X86 "$here/aotpayload.sh" || bad "tests/tools/aotpayload.sh"
# --- the code identity of listed stubs and re-signed native programs (stub-identities, native-identities)
OUT=$OUT/codeid B=$B "$here/codeid.sh" || bad "tests/tools/codeid.sh"
# --- the packaging tools' signature reader and the runtime's give one answer (the fuzz corpus and mutations of it)
if make -s -C "$repo" B="$B" "$B/test_macho" >"$OUT/codesig.build.log" 2>&1 &&
   r=$(python3 -I "$here/test_codesig.py" "$B/test_macho" "$OUT" 2>&1); then pass "code signature readers: $r"
else bad "code signature readers: ${r:-$(tail -3 "$OUT/codesig.build.log")}"; fi
# --- hao-trust-list lists the cdhash of the bytes codesign checked (one read, a private copy)
BX=$(ls -d /System/Applications/*.app/Contents/MacOS/* 2>/dev/null | grep -v ' ' | head -1)   # an Apple bundle's main executable
if r=$(timeout 300 python3 -I "$here/test_trustlist.py" "$G" /bin/ls ${BX:+"$BX"} 2>&1); then
    pass "hao-trust-list: $(echo "$r" | tr '\n' ';')"
else bad "hao-trust-list: $(echo "$r" | grep FAIL)"; fi
# --- --cryptex: a cryptex mounted beside ROOT gives the same trust list, stubs, manifest, log and gate verdicts
# as the cryptex in place under ROOT (tools/hao_root.py)
if r=$(timeout 300 python3 -I "$here/test_root.py" "$OUT/root" "$G" /bin/ls "$B/hao-stub" "$X86" 2>&1); then
    pass "--cryptex: $(echo "$r" | tr '\n' ';')"
else bad "--cryptex: $(echo "$r" | grep FAIL)"; fi
# --- codesign's working copies only under a parent nobody else can change; the directory and files checked by
# inode before codesign reads them
d=$OUT/private; rm -rf "$d"; mkdir -p "$d"
TMPDIR=/private/tmp "$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$d/x.emb" "$G" >"$d/e.log" 2>&1; re=$?
TMPDIR=/private/tmp python3 -I "$repo/tools/hao-trust-list" "$d" "$d/none" >/dev/null 2>"$d/t.log"; rt=$?
python3 -I - "$repo/tools" "$d" <<'PY' >"$d/p.log" 2>&1; rp=$?
import os, shutil, sys
sys.path.insert(0, sys.argv[1])
import hao_codesig
with hao_codesig.PrivateDir("t.") as w:
    f = os.path.join(w.name, "f"); open(f, "w").write("a"); w.created(f); w.check(f)
    os.rename(f, f + ".old"); open(f, "w").write("b")
    try: w.check(f); sys.exit("a replaced file passed")
    except ValueError: pass
    os.rename(w.name, w.name + ".moved"); os.mkdir(w.name, 0o700)
    try: w.check(); sys.exit("a replaced directory passed")
    except ValueError: pass
    shutil.rmtree(w.name + ".moved")
# allow entries with flags (inheritance) grant as much as plain ones; deny entries grant nothing
d = os.path.join(sys.argv[2], "acl"); os.makedirs(d)
n = os.path.join(sys.argv[2], "deny"); os.makedirs(n)
os.system("chmod +a 'everyone allow add_file,directory_inherit' '%s' && mkdir '%s/sub' && "
          "chmod +a 'everyone deny delete' '%s'" % (d, d, n))
assert hao_codesig.acl_grants_write(d) and hao_codesig.acl_grants_write(d + "/sub"), "flagged allow entries"
assert not hao_codesig.acl_grants_write(n), "a deny entry"
os.system("chmod -RN '%s' '%s'" % (d, n))
PY
if [ $re != 0 ] && grep -q "no private place" "$d/e.log" && [ $rt != 0 ] && grep -q "no private place" "$d/t.log" && [ $rp = 0 ]; then
    pass "codesign work: a shared TMPDIR (/private/tmp) is refused; a replaced file or directory fails the check; ACL allow entries with inheritance flags count"
else bad "codesign work: re $re rt $rt rp $rp: $(cat "$d/e.log" "$d/t.log" "$d/p.log")"; fi
echo "tools: $fails failed"
exit $((fails != 0))
