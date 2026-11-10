#!/bin/bash
# Image tools: hao-embed --in-place with hard links and symlinks, hao-install.py
# run twice (idempotent) and its path rules (tests/tools/test_install.py),
# kext/stamp_identity.py's layout checks, and
# tests/tools/bundle.sh (bundle main executables: Info.plist and resource seal bound) and
# tests/tools/stubdeps.sh (the stub links the program's native dependencies; per-stub LC_UUIDs),
# tests/tools/platform.sh (the stub's LC_BUILD_VERSION is the program's: Mac Catalyst, macOS),
# tests/tools/identity.sh (the stub's load-time identity by rule, tools/stub_identity_gate.py) and
# tests/tools/aotpayload.sh (AOT caches shipped in the payload, installed and trusted).
#   OUT=dir [B=build] [KC=BootKernelExtensions.kc] tests/tools/run.sh
# Supply KC explicitly for read-only kernel collection checks; stamp cases
# are skipped without it. Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools)}
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
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/x" --scan >/dev/null 2>"$d/scan.err"
grep -q "needs --root" "$d/scan.err" && pass "--scan without --root: usage error" || bad "--scan without --root: $(cat "$d/scan.err")"

# --- hao-install.py twice on the same System
d=$OUT/install; mkdir -p "$d/src/usr/bin" "$d/sys/usr/bin" "$d/sys/usr/lib/hao" "$d/pay/root/usr/lib/hao"
cp "$G" "$d/src/usr/bin/prog"; cp "$G" "$d/sys/usr/bin/prog"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/pay/embed" --root "$d/src" --x86-cache "$X86" --require usr/bin/prog "$d/src/usr/bin/prog" >/dev/null
python3 -I "$repo/tools/hao-install.py" "$d/sys" "$d/pay" >"$d/1.log" 2>&1; r1=$?
python3 -I "$repo/tools/hao-install.py" "$d/sys" "$d/pay" >"$d/2.log" 2>&1; r2=$?
if [ $r1 = 0 ] && [ $r2 = 0 ] && grep -q '"already_embedded": 1' "$d/sys/usr/lib/hao/install.json"; then
    pass "hao-install.py twice: rc $r1, $r2; second run counts the file as already embedded"
else bad "hao-install.py twice: rc $r1 $r2: $(cat "$d/2.log")"; fi
# --- hao-install.py keeps manifest names under SYSTEM and PAYLOAD and follows no symlink
if python3 -I "$here/test_install.py" >"$d/paths.log" 2>&1; then
    pass "hao-install.py path rules: $(grep '^Ran' "$d/paths.log") (unsafe manifest names and symlinks refused)"
else bad "hao-install.py path rules: $(tail -15 "$d/paths.log")"; fi
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); assert m["inputs_sha256"] and all("stub_sha256" in e for e in m["files"])' \
    "$d/pay/embed/manifest.json" && pass "manifest records per-stub hashes and build inputs" || bad "manifest hashes"

# --- a hard-linked program (27.0 usr/libexec and usr/sbin/appleh13camerad): --scan lists
# both paths, and the installer embeds both whether the image keeps the link or not
d=$OUT/hardlink; rm -rf "$d"; mkdir -p "$d/src/usr/libexec" "$d/src/usr/sbin" "$d/pay/root/usr/lib/hao"
cp "$G" "$d/src/usr/libexec/prog"; ln -f "$d/src/usr/libexec/prog" "$d/src/usr/sbin/prog"
"$repo/tools/hao-embed" --stub "$B/hao-stub" --stubs "$d/pay/embed" --root "$d/src" --x86-cache "$X86" --scan >/dev/null
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
else echo "SKIP stamp cases (no $KC)"; fi
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
echo "tools: $fails failed"
exit $((fails != 0))
