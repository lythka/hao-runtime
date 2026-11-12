#!/bin/bash
# AOT caches shipped in the payload (tools/hao-image-payload --aot / --aot-list) and
# as tools/hao-install.py leaves them in a System tree:
#   OUT=dir [X86_CACHE=dir] tests/tools/aotpayload.sh
# hao-image-payload builds and uses the checkout's build/ (its caches are bound to that
# libhao), so this test runs that build's hao and libhaocore.a too.
# OUT must be a directory tree this user owns whose ancestors are not group- or
# other-writable except sticky ones (hao --aot --cache writes there).
# A scratch ROOT (release 26.0) holds three arm64e builds of tests/guest/hello.c
# at sbin/launchd, usr/libexec/xpcproxy and usr/bin/prog (+ a hard link usr/sbin/prog2).
# Cases:
#  A  embed mode --aot: one cache per file (the hard link shares one), named by the
#     program's arm64e LC_UUID, bound to the payload's libhao and to the program's
#     cdhash (as tools/hao-trust-list computes it), 0644 in a 0755
#     root/usr/lib/hao/aot; build.json aot and aot.json agree
#  B  hao-install.py into a copy of ROOT: caches installed, the cache directory and
#     files get 0755/0644 whatever the payload's modes, a stale cache (another libhao)
#     and one bound to an unsigned guest's code hash are removed; install.json aot_caches
#  C  the installed embedded program (the fat file the installer wrote) under build/hao
#     -v with HAO_CACHE at the installed directory: the cache loads (guest UUID, load
#     commands, cdhash and libhao all match) and the output is the program's; HAO_NO_AOT=1 the same
#  D  root ownership: the installed paths pass the loader's system-directory rules
#     (tests/aot/installed.c) once root-owned, as hao-install.py makes them in the
#     image builder, and fail on the owner alone as this non-root run left them
#  E  --no-embed --aot-list: caches for listed programs only, byte for byte those of A
#     (generation is deterministic and ignores the builder's HAO_* environment);
#     --aot with --no-embed is a usage error; a listed file hao --aot cannot
#     translate fails the build, and so does a path outside ROOT
# Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools-aotpayload)}
B=$repo/build
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
fails=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT" && chmod 755 "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd -P)
uuid_of() { otool -arch arm64e -l "$1" | awk '/cmd LC_UUID/ {getline; getline; print tolower($2); exit}' | tr -d -; }
WANT="hello from arm64e: argc=1 argv0=set pi=3.14159 ok"

# --- the scratch ROOT
R=$OUT/root
mkdir -p "$R/System/Library/CoreServices" "$R/sbin" "$R/usr/libexec" "$R/usr/bin" "$R/usr/sbin"
cat >"$R/System/Library/CoreServices/SystemVersion.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>ProductBuildVersion</key><string>HAOTEST</string>
<key>ProductVersion</key><string>26.0</string></dict></plist>
P
for po in sbin/launchd:0 usr/libexec/xpcproxy:1 usr/bin/prog:s; do
    # distinct programs (distinct LC_UUIDs): each prints the same line
    clang -arch arm64e -O${po#*:} -o "$R/${po%:*}" "$repo/tests/guest/hello.c" && codesign -s - -f "$R/${po%:*}" 2>/dev/null || exit 2
done
ln -f "$R/usr/bin/prog" "$R/usr/sbin/prog2"
U1=$(uuid_of "$R/sbin/launchd") U2=$(uuid_of "$R/usr/libexec/xpcproxy") U3=$(uuid_of "$R/usr/bin/prog")
[ ${#U1} = 32 ] && [ ${#U2} = 32 ] && [ ${#U3} = 32 ] && [ "$U1" != "$U2" ] && [ "$U2" != "$U3" ] && [ "$U1" != "$U3" ] \
    || { echo "aotpayload: test programs need three distinct LC_UUIDs ($U1 $U2 $U3)" >&2; exit 2; }
GEN=$(otool -l "$B/libhao.dylib" | awk '/cmd LC_UUID/ {getline; getline; print tolower($2); exit}' | tr -d -)
printf '%s\n' "$R/sbin/launchd" "$R/usr/libexec/xpcproxy" "$R/usr/bin/prog" "$R/usr/sbin/prog2" >"$OUT/list"

# --- A: embed mode with --aot
P=$OUT/pay
printf '%s\n' '{"fixture":true,"UNKNOWN":3,"scope":"supplied static report"}' >"$OUT/coverage.json"
"$repo/tools/hao-image-payload" --coverage-report "$OUT/coverage.json" --x86-cache "$X86" --aot --aot-jobs 2 "$P" "$R" "$OUT/list" >"$OUT/a.log" 2>&1; rc=$?
A=$P/root/usr/lib/hao/aot
if [ $rc = 0 ]; then pass "A payload --aot: rc 0 ($(grep '^AOT caches' "$OUT/a.log"))"; else bad "A payload --aot rc $rc: $(tail -5 "$OUT/a.log")"; fi
python3 - "$OUT/coverage.json" "$P" <<'PY' && pass "A supplied coverage report preserved" || bad "A supplied coverage report"
import json, pathlib, sys
source, payload = map(pathlib.Path, sys.argv[1:])
expected = json.loads(source.read_text())
assert json.loads((payload / 'census/summary.json').read_text()) == expected
assert json.loads((payload / 'build.json').read_text())['bridge_census'] == expected
PY
got=$(cd "$A" 2>/dev/null && ls | sort | tr '\n' ' ')
want=$(printf '%s.aot\n' "$U1" "$U2" "$U3" | sort | tr '\n' ' ')
[ "$got" = "$want" ] && pass "A one cache per program file, named by its arm64e LC_UUID (the hard link shares one)" \
    || bad "A caches: got [$got] want [$want]"
modes=$(stat -f %Lp "$A" 2>/dev/null; for f in "$A"/*; do stat -f %Lp "$f"; done | sort -u)
[ "$(echo $modes)" = "755 644" ] && pass "A cache directory 0755, caches 0644" || bad "A modes: $(echo $modes)"
python3 - "$P" "$GEN" "$U1" "$U2" "$U3" <<'PY' && pass "A build.json aot and aot.json: 4 programs, 3 files, 3 caches, libhao $GEN, no skips or failures; every header names its guest and this libhao" || bad "A build.json aot"
import json, os, sys
p, gen, *uu = sys.argv[1:]
aes_gen = bytearray.fromhex(gen); aes_gen[15] ^= 1
gen_profiles = {gen, aes_gen.hex()}
b = json.load(open(os.path.join(p, 'build.json')))['aot']
a = json.load(open(os.path.join(p, 'aot.json')))
assert b == a['summary'], 'build.json aot differs from aot.json summary'
assert (b['programs'], b['files'], b['caches'], b['libhao_uuid'], b['skipped'], b['failures'], b['source']) == \
    (4, 3, 3, gen, [], [], 'embedded'), b
d = os.path.join(p, 'root/usr/lib/hao/aot')
assert b['bytes'] == sum(os.path.getsize(os.path.join(d, n)) for n in os.listdir(d)), 'bytes'
assert sorted(x['uuid'] for x in a['programs']) == sorted(uu), a['programs']
assert [x['paths'] for x in a['programs'] if x['uuid'] == uu[2]] == [['usr/bin/prog', 'usr/sbin/prog2']], 'hard link'
for n in os.listdir(d):
    h = open(os.path.join(d, n), 'rb').read(136)
    assert h[:8] == b'HAOAOT2\0' and h[16:32].hex() == n[:32] and h[32:48].hex() in gen_profiles, n
    assert int.from_bytes(h[8:12], 'little') == 7 and int.from_bytes(h[12:16], 'little') == 136, 'format of ' + n
    assert int.from_bytes(h[96:100], 'little') == 1 and not any(h[124:136]), 'code identity kind of ' + n
    assert os.path.relpath(os.path.join(d, n), p) in json.load(open(os.path.join(p, 'build.json')))['files'], 'hash of ' + n
PY

# every cache is bound to its program's cdhash (computed independently by tools/hao-trust-list)
okc=1
for f in sbin/launchd usr/libexec/xpcproxy usr/bin/prog; do
    u=$(uuid_of "$R/$f"); want_cd=$(python3 -I "$repo/tools/hao-trust-list" --cdhash "$R/$f" | awk '{print $1}')
    got_cd=$(xxd -s 104 -l 20 -p "$A/$u.aot" 2>/dev/null)
    [ ${#want_cd} = 40 ] && [ "$got_cd" = "$want_cd" ] || { okc=0; bad "A $f: cache header cdhash [$got_cd], the program's is [$want_cd]"; }
done
[ $okc = 1 ] && pass "A each cache header carries its program's cdhash (tools/hao-trust-list --cdhash)"

# stub-identities: none for these ad hoc programs; with a list that trusts usr/bin/prog's arm slice,
# its stub (and the hard link's) are listed with the cdhash the kernel would report for the stub
SI=$P/root/usr/lib/hao/stub-identities
[ -f "$SI" ] && [ "$(grep -vc '^#' "$SI")" = 0 ] && [ "$(stat -f %Lp "$SI")" = 644 ] \
    && python3 -c 'import json,sys; assert json.load(open(sys.argv[1]))["stub_identities"] == 0' "$P/build.json" \
    && pass "A stub-identities 0644, empty for ad hoc programs (build.json stub_identities 0)" || bad "A stub-identities: $(cat "$SI" 2>&1 | head -3)"
arm=$(python3 -I "$repo/tools/hao-trust-list" --cdhash "$R/usr/bin/prog" | awk '{print $1}')
printf '# test\n%s  usr/bin/prog\n' "$arm" >"$OUT/trusted-prog"
python3 -I "$repo/tools/hao-trust-list" --stubs "$P/embed" "$OUT/trusted-prog" "$R" >"$OUT/stubids" 2>"$OUT/stubids.err"
python3 - "$OUT/stubids" "$P/embed" "$arm" <<'PY' && pass "A --stubs lists exactly the trusted program's stubs: stub cdhash (codesign's), arm cdhash, path" || bad "A --stubs: $(cat "$OUT/stubids" "$OUT/stubids.err" | head -6)"
import json, os, subprocess, sys
lst, embed, arm = sys.argv[1:]
rows = [l.split() for l in open(lst) if not l.startswith('#')]
m = {e['path']: e for e in json.load(open(os.path.join(embed, 'manifest.json')))['files']}
want = sorted(p for p in m if p in ('usr/bin/prog', 'usr/sbin/prog2'))
assert want and sorted(r[3] for r in rows) == want and all(r[2] == 'arm64e' for r in rows), rows
for stub, a, _, p in rows:
    out = subprocess.run(['codesign', '-dvvv', os.path.join(embed, m[p]['stub'])], capture_output=True, text=True).stderr
    cd = [l.split('=', 1)[1] for l in out.splitlines() if l.startswith('CDHash=')]
    assert a == arm and cd == [stub], (stub, a, p, cd)
PY

# --- B: hao-install.py, with modes the installer must fix and a stale cache
S=$OUT/sys
cp -R "$R" "$S"
mkdir -p "$S/usr/lib/hao/aot"
f1=$(ls "$A" | head -1)
python3 - "$A/$f1" "$S/usr/lib/hao/aot/0123456789abcdef0123456789abcdef.aot" "$S/usr/lib/hao/aot/fedcba9876543210fedcba9876543210.aot" <<'PY'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
b[16:32] = bytes.fromhex('0123456789abcdef0123456789abcdef')   # its own name ...
b[32:48] = bytes(16)                                            # ... from another libhao build
open(sys.argv[2], 'wb').write(b)
b = bytearray(open(sys.argv[1], 'rb').read())
b[16:32] = bytes.fromhex('fedcba9876543210fedcba9876543210')   # its own name, this libhao ...
b[96:100] = (2).to_bytes(4, 'little')                           # ... bound to an unsigned guest's code hash
open(sys.argv[3], 'wb').write(b)
PY
chmod 775 "$A"; chmod 664 "$A/$f1"
python3 -I "$P/hao-install.py" "$S" "$P" >"$OUT/b.log" 2>&1; rc=$?
chmod 755 "$A"; chmod 644 "$A/$f1"
SA=$S/usr/lib/hao/aot
[ $rc = 0 ] && pass "B hao-install.py rc 0: $(grep -o 'AOT caches [^;]*' "$OUT/b.log")" || bad "B hao-install.py rc $rc: $(tail -5 "$OUT/b.log")"
got=$(cd "$SA" && ls | sort | tr '\n' ' ')
[ "$got" = "$want" ] && pass "B installed caches are the payload's; the stale one and the one for unsigned code are removed" || bad "B installed: [$got]"
modes=$(stat -f %Lp "$SA"; for f in "$SA"/*; do stat -f %Lp "$f"; done | sort -u)
[ "$(echo $modes)" = "755 644" ] && pass "B installed cache directory 0755, caches 0644 (payload had 0775/0664)" || bad "B modes: $(echo $modes)"
python3 -c '
import json, sys
r = json.load(open(sys.argv[1]))["aot_caches"]
assert (r["caches"], r["libhao_uuid"], r["removed_stale"]) == (3, sys.argv[2], ["0123456789abcdef0123456789abcdef.aot", "fedcba9876543210fedcba9876543210.aot"]), r
' "$S/usr/lib/hao/install.json" "$GEN" && pass "B install.json aot_caches: 3 caches of libhao $GEN, 2 removed (another libhao; unsigned code)" || bad "B install.json aot_caches: $(grep -A6 aot_caches "$S/usr/lib/hao/install.json")"
for f in "$SA"/*.aot; do cmp -s "$f" "$A/$(basename "$f")" || bad "B $(basename "$f") differs from the payload's"; done

# --- C: the installed (embedded) program loads its installed cache
E=$S/usr/bin/prog
lipo -archs "$E" | grep -q x86_64 && pass "C usr/bin/prog was embedded by the installer ($(lipo -archs "$E"))" || bad "C usr/bin/prog not embedded: $(lipo -archs "$E")"
out=$(cd "$OUT" && env HAO_CACHE="$SA" timeout -s KILL 30 "$B/hao" -v "$E" 2>"$OUT/c.err"); rc=$?
if [ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "AOT cache $SA/$U3.aot loaded: [1-9][0-9]* blocks" "$OUT/c.err"; then
    pass "C embedded program: $(grep -o "AOT cache .* loaded: .*" "$OUT/c.err" | sed "s|$SA/||"), rc 7, output as natively"
else bad "C rc $rc out [$out]: $(grep -i aot "$OUT/c.err" | head -3)"; fi
out=$(cd "$OUT" && env HAO_CACHE="$SA" HAO_NO_AOT=1 timeout -s KILL 30 "$B/hao" -v "$E" 2>"$OUT/c2.err"); rc=$?
[ $rc = 7 ] && [ "$out" = "$WANT" ] && ! grep -q "AOT cache" "$OUT/c2.err" && pass "C HAO_NO_AOT=1: no cache read, same output" \
    || bad "C HAO_NO_AOT rc $rc out [$out]"
out=$(cd "$OUT" && env HAO_CACHE="$SA" timeout -s KILL 30 "$B/hao" -v "$S/usr/sbin/prog2" 2>"$OUT/c3.err"); rc=$?
[ $rc = 7 ] && grep -q "AOT cache $SA/$U3.aot loaded" "$OUT/c3.err" && pass "C the hard-linked path loads the same cache" || bad "C prog2 rc $rc"

# --- D: root ownership as the installer gives it in the image builder
clang -arch x86_64 -O1 -I"$repo/include" -o "$OUT/installed" "$repo/tests/aot/installed.c" "$B/libhaocore.a" \
    && codesign -s - -f "$OUT/installed" 2>/dev/null || exit 2
if timeout -s KILL 60 "$OUT/installed" "$S" $(cd "$SA" && ls) >"$OUT/d.log"; then pass "D $(tail -1 "$OUT/d.log" | sed 's/^PASS //')"
else bad "D: $(grep FAIL "$OUT/d.log" | head -5)"; fi
grep -q "^#define HAO_AOT_SYS_RO_DIR \"/usr/lib/hao/aot\"" "$repo/include/hao/aot.h" && grep -q '^AOT_DIRS = \["usr/lib/hao/aot"' "$repo/tools/hao-install.py" \
    && grep -q 'OUT/root/usr/lib/hao/aot' "$repo/tools/hao-image-payload" \
    && pass "D the payload, the installer and the loader name the same directory (/usr/lib/hao/aot)" || bad "D directory names differ"

# --- E: --no-embed --aot-list; usage and fail-closed cases
printf '# comment\n/usr/bin/prog\nusr/libexec/xpcproxy\n%s\n' "$R/usr/bin/prog" >"$OUT/alist"
P2=$OUT/pay2
env HAO_NO_CHAIN=1 HAO_JIT_HELPERS_ONLY=1 HAO_CACHE=/nonexistent "$repo/tools/hao-image-payload" --no-embed --aot-list "$OUT/alist" "$P2" "$R" >"$OUT/e.log" 2>&1; rc=$?
got=$(cd "$P2/root/usr/lib/hao/aot" 2>/dev/null && ls | sort | tr '\n' ' ')
want2=$(printf '%s.aot\n' "$U2" "$U3" | sort | tr '\n' ' ')
if [ $rc = 0 ] && [ "$got" = "$want2" ] && cmp -s "$P2/root/usr/lib/hao/aot/$U3.aot" "$A/$U3.aot" && cmp -s "$P2/root/usr/lib/hao/aot/$U2.aot" "$A/$U2.aot" \
    && python3 -c 'import json,sys; b=json.load(open(sys.argv[1]))["aot"]; assert (b["source"], b["programs"], b["caches"]) == ("list", 2, 2), b' "$P2/build.json"; then
    pass "E --no-embed --aot-list: the 2 listed programs (3 lines, 1 repeat), byte for byte A's caches with HAO_NO_CHAIN/HAO_JIT_HELPERS_ONLY/HAO_CACHE in the builder's environment"
else bad "E --aot-list rc $rc [$got]: $(tail -3 "$OUT/e.log")"; fi
"$repo/tools/hao-image-payload" --no-embed --aot "$OUT/pay3" "$R" >"$OUT/e3.log" 2>&1; rc=$?
[ $rc = 2 ] && grep -q "use --aot-list" "$OUT/e3.log" && [ ! -e "$OUT/pay3" ] && pass "E --no-embed --aot: usage error (rc 2)" || bad "E --no-embed --aot rc $rc: $(tail -2 "$OUT/e3.log")"
printf '#!/bin/sh\nexit 0\n' >"$R/usr/bin/script"; chmod 755 "$R/usr/bin/script"
printf 'usr/bin/prog\nusr/bin/script\n' >"$OUT/alist4"
"$repo/tools/hao-image-payload" --no-embed --aot-list "$OUT/alist4" "$OUT/pay4" "$R" >"$OUT/e4.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q "AOT: usr/bin/script: hao --aot rc" "$OUT/e4.log" && pass "E a listed file hao --aot fails on fails the build (rc 1): $(grep -o 'AOT: usr/bin/script: .*' "$OUT/e4.log" | cut -c1-120)" \
    || bad "E failing program rc $rc: $(tail -3 "$OUT/e4.log")"
printf '/etc/../../outside\n' >"$OUT/alist5"
ln -sf /bin/ls "$R/usr/bin/escape"; printf 'usr/bin/escape\n' >>"$OUT/alist5"
"$repo/tools/hao-image-payload" --no-embed --aot-list "$OUT/alist5" "$OUT/pay5" "$R" >"$OUT/e5.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q "not a file under ROOT" "$OUT/e5.log" && pass "E paths that leave ROOT (.., a symlink out) fail the build (rc 1)" || bad "E outside ROOT rc $rc: $(tail -3 "$OUT/e5.log")"
rm -f "$R/usr/bin/escape" "$R/usr/bin/script"
echo "aotpayload: $fails failed"
exit $((fails != 0))
