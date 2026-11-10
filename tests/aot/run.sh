#!/bin/bash
# AOT cache trust and validation, end to end through build/hao:
#   OUT=dir [HAO=build/hao] [B=build] tests/aot/run.sh
# OUT must be a directory tree this user owns with no group/other-writable
# ancestors except sticky ones (a scratch directory, not /tmp itself).
# Builds tests/guest/hello.c for arm64e (exit 7) and checks that hao loads a
# valid cache and rejects every untrusted or malformed one while still
# running the program correctly. Case T is the twin: a file with the
# program's mach header, load commands and LC_UUID but other code, whose
# cache the program must not run. Also the other rules of the environment
# tiers that need a running guest: dependency paths in a set-ID process (L)
# and the diagnostics log's lock and mode (M). Prints PASS/FAIL per case;
# exit 1 on FAIL.
# Writes nothing outside OUT except case H, which runs `hao --aot` with no
# HOME, no --cache and no HAO_CACHE and removes the one cache file it creates in
# ~/Library/Caches/hao.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
HAO=${HAO:-$repo/build/hao}
B=${B:-$(dirname "$HAO")}
OUT=${OUT:?set OUT to a scratch directory}
case $OUT in /|"$HOME"|"$HOME/"|/tmp|/private/tmp) echo "OUT=$OUT: use a directory of its own" >&2; exit 2 ;; esac
T=${T:-30}
rm -rf "$OUT"
mkdir -p "$OUT" && chmod 755 "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd -P)
fail=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

G=$OUT/hello.arm64e
clang -arch arm64e -O1 -o "$G" "$repo/tests/guest/hello.c" && codesign -s - -f "$G" 2>/dev/null || exit 1
WANT="hello from arm64e: argc=1 argv0=set pi=3.14159 ok"

# run NAME [env...] -- runs $HAO $G with HAO_VERBOSE=1 plus the given env;
# sets rc, out, err
run() {
    local name=$1; shift
    env HAO_VERBOSE=1 "$@" timeout -s KILL "$T" "$HAO" "$G" >"$OUT/$name.out" 2>"$OUT/$name.err"
    rc=$?
    out=$(cat "$OUT/$name.out")
    err=$(cat "$OUT/$name.err")
}
# expect_reject NAME DIR REASON: the cache in DIR is rejected for REASON and
# the program still runs correctly (JIT)
expect_reject() {
    local name=$1 dir=$2 reason=$3
    run "$name" HAO_CACHE="$dir"
    if [ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "rejected: .*$reason" "$OUT/$name.err" && ! grep -q "loaded:" "$OUT/$name.err"; then
        pass "$name: rejected ($(grep -o 'rejected: .*' "$OUT/$name.err" | head -1 | cut -c11-)), rc 7"
    else
        bad "$name: rc=$rc out='$out' err: $(grep -E 'AOT|rejected' "$OUT/$name.err" | head -2)"
    fi
}
# fresh DIR [MODE]: an empty cache directory
fresh() { rm -rf "$1"; mkdir -p "$1"; chmod "${2:-700}" "$1"; }

# --- B: a valid cache loads and runs
C=$OUT/c
fresh "$C"
if ! timeout -s KILL "$T" "$HAO" --aot --cache $C "$G" >"$OUT/gen.err" 2>&1; then bad "B generate: $(cat "$OUT/gen.err")"; fi
AOT=$(ls "$C"/*.aot 2>/dev/null | head -1)
NAME=$(basename "${AOT:-none}")
run good HAO_CACHE="$C"
if [ -n "$AOT" ] && [ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "AOT cache .*$NAME loaded" "$OUT/good.err"; then
    pass "B valid cache: $(grep -o 'loaded: .*' "$OUT/good.err"), rc 7"
else
    bad "B valid cache: rc=$rc out='$out' err=$(head -3 "$OUT/good.err")"
fi
[ -n "$AOT" ] || { echo "no cache generated; stopping"; exit 1; }
# a cache in a 0700 directory under a sticky ancestor (OUT/st is 1777)
mkdir -p "$OUT/st" && chmod 1777 "$OUT/st" && fresh "$OUT/st/c" && cp "$AOT" "$OUT/st/c/"
run sticky HAO_CACHE="$OUT/st/c"
if [ $rc = 7 ] && grep -q "loaded:" "$OUT/sticky.err"; then pass "B2 0700 dir under a sticky 1777 dir loads"; else bad "B2 sticky ancestor: rc=$rc $(head -2 "$OUT/sticky.err")"; fi

# --- D: generated file mode does not depend on the umask
fresh "$OUT/u"
(umask 000; timeout -s KILL "$T" "$HAO" --aot --cache $OUT/u "$G" >/dev/null 2>&1)
m=$(stat -f %Lp "$OUT/u/$NAME" 2>/dev/null)
[ "$m" = 644 ] && pass "D umask 000: generated cache is mode $m" || bad "D umask 000: mode '$m'"

# --- C: ownership, mode and symlink rejections
fresh "$OUT/c1"; cp "$AOT" "$OUT/c1/"; chmod 666 "$OUT/c1/$NAME"
expect_reject C1-file-0666 "$OUT/c1" "writable by group or others"
fresh "$OUT/c2" 777; cp "$AOT" "$OUT/c2/"
expect_reject C2-dir-0777 "$OUT/c2" "writable by group or others"
fresh "$OUT/c2s" 1777; cp "$AOT" "$OUT/c2s/"
expect_reject C2s-dir-1777 "$OUT/c2s" "writable by group or others"
mkdir -p "$OUT/gw" && chmod 775 "$OUT/gw" && fresh "$OUT/gw/c" && cp "$AOT" "$OUT/gw/c/"
expect_reject C3-ancestor-0775 "$OUT/gw/c" "writable by group or others"
fresh "$OUT/c3a"; cp "$AOT" "$OUT/c3a/"; chmod +a "group:everyone allow add_file,delete_child" "$OUT/c3a"
expect_reject C3a-dir-acl-write "$OUT/c3a" "ACL grants write access"
fresh "$OUT/c3r"; cp "$AOT" "$OUT/c3r/"; chmod +a "group:everyone allow list,search,readattr" "$OUT/c3r"
run c3r HAO_CACHE="$OUT/c3r"
[ $rc = 7 ] && grep -q "loaded:" "$OUT/c3r.err" && pass "C3r read-only ACL grant on the dir: loads" || bad "C3r read ACL: rc=$rc $(head -2 "$OUT/c3r.err")"
fresh "$OUT/c4"; ln -s "$AOT" "$OUT/c4/$NAME"
expect_reject C4-symlinked-file "$OUT/c4" "symlink"
ln -s "$C" "$OUT/clink"
expect_reject C5-symlinked-dir "$OUT/clink" "symlink owned by uid"
fresh "$OUT/c5f"; mkfifo "$OUT/c5f/$NAME"
expect_reject C5f-fifo "$OUT/c5f" "not a regular file"

# --- C: format rejections (patch.py recomputes the payload hash unless nohash)
P="python3 -I $here/patch.py"
patched() {  # patched CASE REASON EDIT...
    local name=$1 reason=$2; shift 2
    fresh "$OUT/$name"
    $P "$AOT" "$OUT/$name/$NAME" "$@" || { bad "$name: patch.py failed"; return; }
    expect_reject "$name" "$OUT/$name" "$reason"
}
patched C6-code-int3-hash-not-fixed "payload hash mismatch" plant nohash
patched C6b-code-byte-hash-not-fixed "payload hash mismatch" flip.code nohash
patched C7-code_off-0x7ffffff0 "outside the code area" ent.code_off=0:0x7ffffff0
patched C7b-code_off-misaligned "not 16-aligned" ent.code_off=1:+8
patched C7c-code_off-in-base-slot "outside the code area" ent.code_off=0:0
patched C8-guest_off-0xfffffff0 "outside the image" ent.guest_off=0:0xfffffff0
patched C8b-guest_off-misaligned "not 4-aligned" ent.guest_off=0:+2
patched C9-code_size-wrap "do not add up" hdr.code_size=0xFFFFFFFFFFFF0000
patched C9b-code_size-plus-16 "do not add up" hdr.code_size=+16
patched C9c-nblocks-huge "blocks for" hdr.nblocks=0xffffffff
patched C9d-nlinks-huge "do not add up" hdr.nlinks=0x40000000
patched C9e-file_size "header says" hdr.file_size=+1
patched C10-truncated-half "header says" truncate=$(( $(stat -f %z "$AOT") / 2 ))
patched C10b-truncated-header "truncated" truncate=40
patched C11-other-build-gen-byte "another hao build" flip.gen
patched C11b-version-4 "format version" hdr.version=4
patched C12-guest-uuid "guest UUID differs" flip.uuid
patched C12b-guest-id "guest load commands differ" hdr.guest_id=+1
patched C12c-vmsize "vmsize" hdr.vmsize=+0x4000
patched C12d-code-identity "guest code differs: this image's cdhash is" flip.code_id
patched C12e-identity-hash-type "guest code differs" hdr.id_hash=+1
patched C12f-identity-of-an-unsigned-guest "translated from an unsigned guest" hdr.id_kind=2
patched C12g-identity-kind-unknown "unknown code identity kind" hdr.id_kind=9
patched C13-link-target "target block" link.target=0:0xffff
patched C13b-link-site "outside the code area" link.site=0:0xfffffff0
patched C13c-link-site-moved "does not reach" link.site=0:+1

# --- T: the twin. A file that copies the program's mach header, load
# commands and LC_UUID but holds other code (here one instruction: exit 9
# instead of 7) gives `hao --aot` everything a cache is named by. The program
# must refuse the twin's cache, say why, and run its own code (rc 7); the
# twin itself still loads it (rc 9), which shows the cache is otherwise good.
TW=$OUT/twin; mkdir -p "$TW"
ident=$(codesign -dv "$G" 2>&1 | sed -n 's/^Identifier=//p')
# twin SRC DST: DST = SRC with `mov w0, #7` (the return value) made `mov w0, #9`
twin() {
    python3 -I - "$1" "$2" <<'EOF'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
old, new = bytes.fromhex("e0008052"), bytes.fromhex("20018052")     # mov w0, #7 / mov w0, #9
if d.count(old) != 1:
    sys.exit("twin: expected one `mov w0, #7`, found %d" % d.count(old))
i = d.index(old)
d[i:i + 4] = new
open(sys.argv[2], "wb").write(d)
EOF
}
# same_cmds A B: the two files have the same mach header and load commands (so the same LC_UUID)
same_cmds() {
    python3 -I - "$1" "$2" <<'EOF'
import struct, sys
a, b = (open(p, "rb").read() for p in sys.argv[1:3])
n = 32 + struct.unpack_from("<I", a, 20)[0]
sys.exit(0 if a[:4] == bytes.fromhex("cffaedfe") and a[:n] == b[:n] and len(a) == len(b) and a != b else 1)
EOF
}
# T1: the twin keeps the program's signature, which no longer matches its code: no cache at all
twin "$G" "$TW/hello.stolen-sig" && chmod 755 "$TW/hello.stolen-sig" || bad "T1 make twin"
fresh "$OUT/t1"
timeout -s KILL "$T" "$HAO" --aot --cache $OUT/t1 "$TW/hello.stolen-sig" >"$OUT/t1.err" 2>&1; rc=$?
if same_cmds "$G" "$TW/hello.stolen-sig" && [ $rc != 0 ] && grep -q "code signature invalid (page .* does not match its hash): no cache written" "$OUT/t1.err" \
   && [ -z "$(ls -A "$OUT/t1")" ]; then
    pass "T1 twin with the program's signature: hao --aot refuses ($(sed 's/^hao-aot: [^:]*: //' "$OUT/t1.err" | head -1)), directory left empty"
else bad "T1 twin with stolen signature: rc=$rc $(head -2 "$OUT/t1.err") dir: $(ls -A "$OUT/t1")"; fi
# T2: the twin is signed again (ad hoc, same identifier: the signature keeps its size, so the load
# commands stay the program's). Its cache is valid for the twin and must be refused by the program.
cp "$TW/hello.stolen-sig" "$TW/hello.arm64e" && codesign -s - -f -i "$ident" "$TW/hello.arm64e" 2>/dev/null || bad "T2 sign twin"
fresh "$OUT/t2"
timeout -s KILL "$T" "$HAO" --aot --cache $OUT/t2 "$TW/hello.arm64e" >"$OUT/t2.gen" 2>&1; rc=$?
if same_cmds "$G" "$TW/hello.arm64e" && [ $rc = 0 ] && [ "$(ls -A "$OUT/t2")" = "$NAME" ]; then
    pass "T2 signed twin: same mach header, load commands and LC_UUID as the program, other code; hao --aot writes $NAME"
else bad "T2 signed twin: rc=$rc same_cmds=$(same_cmds "$G" "$TW/hello.arm64e"; echo $?) $(head -2 "$OUT/t2.gen") dir: $(ls -A "$OUT/t2")"; fi
env HAO_VERBOSE=1 HAO_CACHE="$OUT/t2" timeout -s KILL "$T" "$HAO" "$TW/hello.arm64e" >"$OUT/t2t.out" 2>"$OUT/t2t.err"; rc=$?
[ $rc = 9 ] && grep -q "loaded:" "$OUT/t2t.err" && pass "T2 control: the twin loads its own cache and runs its own code (rc 9)" \
    || bad "T2 control: rc=$rc $(grep AOT "$OUT/t2t.err" | head -2)"
expect_reject T2-program-refuses-the-twins-cache "$OUT/t2" "guest code differs: this image's cdhash is [0-9a-f]\{40\} (hash type 2), the cache was translated from [0-9a-f]\{40\}"
# T3: the same with unsigned files (run with --allow-unsigned): the identity is the hash of the mapped code
U=$OUT/unsigned; mkdir -p "$U"
clang -arch arm64e -O1 -Wl,-no_adhoc_codesign -o "$U/hello.arm64e" "$repo/tests/guest/hello.c" && twin "$U/hello.arm64e" "$U/twin.arm64e" \
    && chmod 755 "$U/twin.arm64e" || bad "T3 build unsigned"
fresh "$OUT/t3"
timeout -s KILL "$T" "$HAO" --aot --cache $OUT/t3 "$U/hello.arm64e" >"$OUT/t3.gen" 2>&1; rc=$?
env HAO_VERBOSE=1 HAO_CACHE="$OUT/t3" timeout -s KILL "$T" "$HAO" --allow-unsigned "$U/hello.arm64e" >"$OUT/t3.out" 2>"$OUT/t3.err"; rc2=$?
[ $rc = 0 ] && [ $rc2 = 7 ] && grep -q "loaded:" "$OUT/t3.err" && pass "T3 unsigned program: its own cache (code-hash identity) loads, rc 7" \
    || bad "T3 unsigned own cache: gen rc=$rc run rc=$rc2 $(head -1 "$OUT/t3.gen") $(grep AOT "$OUT/t3.err" | head -2)"
fresh "$OUT/t3t"
timeout -s KILL "$T" "$HAO" --aot --cache $OUT/t3t "$U/twin.arm64e" >"$OUT/t3t.gen" 2>&1; rc=$?
env HAO_VERBOSE=1 HAO_CACHE="$OUT/t3t" timeout -s KILL "$T" "$HAO" --allow-unsigned "$U/hello.arm64e" >"$OUT/t3t.out" 2>"$OUT/t3t.err"; rc2=$?
if same_cmds "$U/hello.arm64e" "$U/twin.arm64e" && [ $rc = 0 ] && [ $rc2 = 7 ] && [ "$(cat "$OUT/t3t.out")" = "$WANT" ] \
   && grep -q "rejected: guest code differs: this image's code hash is" "$OUT/t3t.err" && ! grep -q "loaded:" "$OUT/t3t.err"; then
    pass "T3 unsigned twin's cache: rejected ($(grep -o 'rejected: .*' "$OUT/t3t.err" | head -1 | cut -c11-)), rc 7"
else bad "T3 unsigned twin: gen rc=$rc run rc=$rc2 $(grep -E 'AOT|rejected' "$OUT/t3t.err" | head -2)"; fi
# T4: the kind of identity follows from the image, not from the file: the unsigned program's own
# cache, relabelled as a signed guest's, is not taken
UN=$(ls "$OUT/t3" | head -1)
fresh "$OUT/t4"; $P "$OUT/t3/$UN" "$OUT/t4/$UN" hdr.id_kind=1
env HAO_VERBOSE=1 HAO_CACHE="$OUT/t4" timeout -s KILL "$T" "$HAO" --allow-unsigned "$U/hello.arm64e" >"$OUT/t4.out" 2>"$OUT/t4.err"; rc=$?
[ $rc = 7 ] && grep -q "rejected: translated from a code-signed guest; this image is not signed" "$OUT/t4.err" && ! grep -q "loaded:" "$OUT/t4.err" \
    && pass "T4 an unsigned program's cache relabelled as a signed guest's: rejected ($(grep -o 'rejected: .*' "$OUT/t4.err" | head -1 | cut -c11-)), rc 7" \
    || bad "T4: rc=$rc $(grep AOT "$OUT/t4.err" | head -2)"
# T5: hao --aot leaves nothing but the cache behind (its private copy of the guest is gone)
[ "$(ls -A "$C")" = "$NAME" ] && pass "T5 hao --aot leaves only $NAME in the cache directory" || bad "T5 leftovers: $(ls -A "$C")"
# T6: the file is rewritten in place while hao --aot works on it (its owner flips it between the
# program and the twin that keeps the program's signature). hao translates a private copy, so every
# run either refuses (the copy caught the twin) or writes exactly the program's cache: never the
# program's cdhash over the twin's code.
cp "$G" "$TW/flip" && fresh "$OUT/t6"
python3 -I - "$G" "$TW/hello.stolen-sig" "$TW/flip" <<'EOF' &
import sys
a, b = (open(p, "rb").read() for p in sys.argv[1:3])
at = [i for i in range(len(a)) if a[i] != b[i]]
lo, hi = at[0], at[-1] + 1
f = open(sys.argv[3], "r+b", buffering=0)
while True:
    for v in (b, a):
        f.seek(lo)
        f.write(v[lo:hi])
EOF
flipper=$!
wrote=0 refused=0 wrong=0
for i in $(seq 60); do
    rm -f "$OUT/t6/$NAME"
    timeout -s KILL "$T" "$HAO" --aot --cache $OUT/t6 "$TW/flip" >"$OUT/t6.err" 2>&1; rc=$?
    if [ -e "$OUT/t6/$NAME" ]; then
        if [ $rc = 0 ] && cmp -s "$OUT/t6/$NAME" "$AOT"; then wrote=$((wrote + 1)); else wrong=$((wrong + 1)); fi
    elif [ $rc != 0 ] && grep -q "code signature invalid" "$OUT/t6.err"; then refused=$((refused + 1))
    else wrong=$((wrong + 1)); fi
done
kill $flipper 2>/dev/null; wait $flipper 2>/dev/null
[ $wrong = 0 ] && [ "$(ls -A "$OUT/t6" | grep -vx "$NAME")" = "" ] \
    && pass "T6 file rewritten in place during hao --aot, 60 runs: $wrote wrote the program's exact cache, $refused refused, 0 other" \
    || bad "T6 rewritten in place: $wrote good, $refused refused, $wrong wrong; last: $(head -1 "$OUT/t6.err"); dir: $(ls -A "$OUT/t6")"

# a cache written by another hao build: a copy of the build whose libhao
# LC_UUID differs (one byte), as any rebuild with other code gives
mkdir -p "$OUT/other" && cp "$B/hao" "$B/libhao.dylib" "$B/libhao_identity.dylib" "$OUT/other/" || bad "C11c copy build"
python3 -I - "$OUT/other/libhao.dylib" <<'EOF'
import struct, sys
p = sys.argv[1]; d = bytearray(open(p, "rb").read())
ncmds = struct.unpack_from("<I", d, 16)[0]; off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", d, off)
    if cmd == 0x1b:                       # LC_UUID
        d[off + 8] ^= 0x5a; break
    off += size
else:
    sys.exit("no LC_UUID")
open(p, "wb").write(d)
EOF
codesign -s - -f "$OUT/other/libhao.dylib" 2>/dev/null; codesign -s - -f "$OUT/other/libhao_identity.dylib" 2>/dev/null
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$OUT/other/hao" 2>/dev/null
fresh "$OUT/c11c"
timeout -s KILL "$T" "$OUT/other/hao" --aot --cache $OUT/c11c "$G" >/dev/null 2>&1 || bad "C11c generate with the other build"
expect_reject C11c-cache-from-another-build "$OUT/c11c" "another hao build"

# relative HAO_CACHE: not used at all
(cd "$OUT" && env HAO_VERBOSE=1 HAO_CACHE=c timeout -s KILL "$T" "$HAO" "$G" >"$OUT/c14.out" 2>"$OUT/c14.err"); rc=$?
if [ $rc = 7 ] && grep -q "not absolute; AOT is off" "$OUT/c14.err" && ! grep -q "loaded:" "$OUT/c14.err"; then
    pass "C14 relative HAO_CACHE: AOT off, rc 7"
else bad "C14 relative HAO_CACHE: rc=$rc $(head -2 "$OUT/c14.err")"; fi

# --- K: `hao --aot --cache DIR` names the output on the command line. A guarded process
# (root is one) ignores HAO_CACHE, and DIR passes the same trust rules as HAO_CACHE's directory.
fresh "$OUT/k1"; fresh "$OUT/k1e"
env HAO_DISTRUST_ENV=1 HAO_CACHE="$OUT/k1e" timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/k1" "$G" >"$OUT/k1.err" 2>&1; rc=$?
[ $rc = 0 ] && cmp -s "$OUT/k1/$NAME" "$AOT" && [ -z "$(ls -A "$OUT/k1e")" ] && grep -q -- "-> $OUT/k1/$NAME\$" "$OUT/k1.err" \
    && pass "K1 guarded process: --cache DIR written (the same bytes as B's cache), HAO_CACHE untouched" \
    || bad "K1 guarded --cache: rc=$rc $(tail -2 "$OUT/k1.err")"
env HAO_DISTRUST_ENV=1 HAO_CACHE="$OUT/k1e" timeout -s KILL "$T" "$HAO" --aot "$G" >"$OUT/k2.err" 2>&1; rc=$?
[ $rc != 0 ] && [ -z "$(ls -A "$OUT/k1e")" ] && grep -q "no trusted AOT cache directory" "$OUT/k2.err" \
    && pass "K2 guarded process without --cache: HAO_CACHE ignored, nothing written there (rc $rc)" \
    || bad "K2 guarded HAO_CACHE: rc=$rc $(tail -2 "$OUT/k2.err")"
fresh "$OUT/k3"
env HAO_DISTRUST_ENV=2 timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/k3" "$G" >"$OUT/k3.err" 2>&1; rc=$?
[ $rc != 0 ] && [ -z "$(ls -A "$OUT/k3")" ] && grep -q "setuid/setgid" "$OUT/k3.err" && pass "K3 set-ID tier: --cache refused (rc $rc)" \
    || bad "K3 set-ID --cache: rc=$rc $(tail -2 "$OUT/k3.err")"
fresh "$OUT/k4" 775
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/k4" "$G" >"$OUT/k4.err" 2>&1; rc=$?
[ $rc != 0 ] && [ -z "$(ls -A "$OUT/k4")" ] && grep -q "rejected: .*writable by group or others" "$OUT/k4.err" \
    && pass "K4 --cache names a 0775 directory: rejected, nothing written" || bad "K4 --cache 0775: rc=$rc $(tail -2 "$OUT/k4.err")"
fresh "$OUT/k5t"; ln -s "$OUT/k5t" "$OUT/k5"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/k5" "$G" >"$OUT/k5.err" 2>&1; rc=$?
[ $rc != 0 ] && [ -z "$(ls -A "$OUT/k5t")" ] && grep -q "symlink owned by uid" "$OUT/k5.err" \
    && pass "K5 --cache through the user's own symlink: rejected, nothing written" || bad "K5 --cache symlink: rc=$rc $(tail -2 "$OUT/k5.err")"
fresh "$OUT/k6"
(cd "$OUT" && timeout -s KILL "$T" "$HAO" --aot --cache k6/new -- "$G" >"$OUT/k6.err" 2>&1); rc=$?
[ $rc = 0 ] && cmp -s "$OUT/k6/new/$NAME" "$AOT" && pass "K6 relative --cache: taken from the working directory, created, written" \
    || bad "K6 relative --cache: rc=$rc $(tail -2 "$OUT/k6.err")"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/k6" >"$OUT/k7.err" 2>&1; rc=$?
[ $rc = 64 ] && grep -q -- "--aot \[--cache DIR\]" "$OUT/k7.err" && pass "K7 --cache without a file: usage, rc 64" || bad "K7 usage: rc=$rc $(head -2 "$OUT/k7.err")"

# --- E: a planted, well-formed cache (int3 code, hash fixed) would run if
# trusted (control), and is never reached from an untrusted environment
PL=$OUT/planted; fresh "$PL"; $P "$AOT" "$PL/$NAME" plant
run e0 HAO_CACHE="$PL"
[ $rc = 133 ] && grep -q "loaded:" "$OUT/e0.err" && pass "E0 control: planted cache in a trusted HAO_CACHE runs (rc 133, SIGTRAP)" \
    || bad "E0 control: rc=$rc $(head -2 "$OUT/e0.err")"
run e1 HAO_DISTRUST_ENV=1 HAO_CACHE="$PL"
[ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "HAO_CACHE ignored (guarded process)" "$OUT/e1.err" && ! grep -q "loaded:" "$OUT/e1.err" \
    && pass "E1 guarded process: HAO_CACHE ignored, rc 7" || bad "E1 guarded: rc=$rc $(head -2 "$OUT/e1.err")"
run e2 HAO_DISTRUST_ENV=2 HAO_CACHE="$PL"
[ $rc = 7 ] && [ "$out" = "$WANT" ] && ! grep -q "loaded:" "$OUT/e2.err" && pass "E2 setugid tier: HAO_CACHE ignored, rc 7" \
    || bad "E2 setugid tier: rc=$rc $(head -2 "$OUT/e2.err")"
# guarded: CLI flags (passed to stage 2 as HAO_FLAGS) and HAO_AOT_LEARN are ignored
env HAO_VERBOSE=1 HAO_DISTRUST_ENV=1 HAO_AOT_LEARN=1 timeout -s KILL "$T" "$HAO" --stats "$G" >"$OUT/e2g.out" 2>"$OUT/e2g.err"; rc=$?
env HAO_VERBOSE=1 timeout -s KILL "$T" "$HAO" --stats "$G" >/dev/null 2>"$OUT/e2o.err"
if [ $rc = 7 ] && grep -q "HAO_AOT_LEARN ignored (guarded process)" "$OUT/e2g.err" && ! grep -q "^hao: blocks=" "$OUT/e2g.err" \
   && grep -q "^hao: blocks=" "$OUT/e2o.err"; then
    pass "E2g guarded: --stats (HAO_FLAGS) and HAO_AOT_LEARN ignored, rc 7 (open: --stats prints)"
else bad "E2g: rc=$rc $(grep -E 'ignored|blocks=' "$OUT/e2g.err" | head -3)"; fi

# a real setgid exec: a copy of hao (libhao and libhao_identity by absolute
# path: dyld ignores @executable_path rpaths in a set-ID process) that is
# setgid to a group this user is in but that is not the primary group, so
# egid != gid (the kernel leaves issetugid() clear for a group the caller
# belongs to; a setuid-root copy would need root)
SG=$OUT/sg; mkdir -p "$SG"; cp "$B/hao" "$B/libhao.dylib" "$B/libhao_identity.dylib" "$SG/"
install_name_tool -change @rpath/libhao.dylib "$SG/libhao.dylib" -change @rpath/libhao_identity.dylib \
    "$SG/libhao_identity.dylib" "$SG/hao" 2>/dev/null
codesign -s - -f "$SG/libhao.dylib" 2>/dev/null; codesign -s - -f "$SG/libhao_identity.dylib" 2>/dev/null
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$SG/hao" 2>/dev/null
grp=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
env HAO_CACHE="$PL" timeout -s KILL "$T" "$SG/hao" "$G" >/dev/null 2>&1; rc0=$?
chgrp "$grp" "$SG/hao" && chmod g+s "$SG/hao"
env HAO_CACHE="$PL" timeout -s KILL "$T" "$SG/hao" "$G" >"$OUT/e3.out" 2>"$OUT/e3.err"; rc=$?
if [ $rc0 = 133 ] && [ $rc = 7 ] && [ "$(cat "$OUT/e3.out")" = "$WANT" ]; then
    pass "E3 setgid hao (egid $grp != gid $(id -g)): planted HAO_CACHE ignored, rc 7 (same copy without setgid: rc $rc0)"
else bad "E3 setgid: rc without setgid=$rc0 (want 133), with setgid=$rc (want 7) $(head -2 "$OUT/e3.err")"; fi
# the same for an embedded program (the stub form, src/main.c hao_self_main):
# the setgid file is the program itself (src/main.c)
"$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$SG/hello.emb" "$G" >/dev/null 2>&1
env HAO_CACHE="$PL" timeout -s KILL "$T" arch -x86_64 "$SG/hello.emb" >/dev/null 2>&1; rc0=$?
chgrp "$grp" "$SG/hello.emb" && chmod g+s "$SG/hello.emb"
env HAO_CACHE="$PL" timeout -s KILL "$T" arch -x86_64 "$SG/hello.emb" >"$OUT/e3e.out" 2>"$OUT/e3e.err"; rc=$?
if [ $rc0 = 133 ] && [ $rc = 7 ] && [ "$(cat "$OUT/e3e.out")" = "$WANT" ]; then
    pass "E3e setgid embedded program: planted HAO_CACHE ignored, rc 7 (same file without setgid: rc $rc0)"
else bad "E3e setgid embedded: rc without setgid=$rc0 (want 133), with setgid=$rc (want 7) $(head -2 "$OUT/e3e.err")"; fi

# --- G: learn mode does not follow a symlinked hints file
fresh "$OUT/lg"; cp "$AOT" "$OUT/lg/"
echo victim > "$OUT/victim"; ln -s "$OUT/victim" "$OUT/lg/${NAME%.aot}.hints"
run g HAO_CACHE="$OUT/lg" HAO_AOT_LEARN=1
[ "$(cat "$OUT/victim")" = victim ] && [ $rc = 7 ] && pass "G HAO_AOT_LEARN: symlinked hints file not written through" \
    || bad "G learn: rc=$rc victim=$(cat "$OUT/victim")"

# --- H: no HOME, no HAO_CACHE: the per-user directory from the user database,
# never /tmp or /Library/Caches
pw=$(python3 -I -c 'import os,pwd; print(pwd.getpwuid(os.geteuid()).pw_dir)')
pre=0; [ -e "$pw/Library/Caches/hao/$NAME" ] && pre=1
env -u HOME -u HAO_CACHE timeout -s KILL "$T" "$HAO" --aot "$G" >"$OUT/h.err" 2>&1; rc=$?
if [ $rc = 0 ] && grep -q -- "-> $pw/Library/Caches/hao/$NAME\$" "$OUT/h.err" && ! grep -qE -- "-> (/private)?/tmp/|-> /Library" "$OUT/h.err"; then
    pass "H no HOME: wrote $pw/Library/Caches/hao/$NAME"
else bad "H no HOME: rc=$rc $(cat "$OUT/h.err")"; fi
[ $pre = 1 ] || rm -f "$pw/Library/Caches/hao/$NAME"

# --- I: hao-install.py creates the system cache directories 0755 (and
# root:wheel when run as root, as in the image builder) and rejects a symlink
I=$OUT/inst; mkdir -p "$I/sys/usr/lib" "$I/sys/private/var/db" "$I/pay/root/usr/lib/hao/aot" "$I/pay/embed"
echo '{"files": [], "required": [], "stub_sha256": ""}' > "$I/pay/embed/manifest.json"
python3 -I "$repo/tools/hao-install.py" "$I/sys" "$I/pay" >"$I/log" 2>&1; rc=$?
m1=$(stat -f %Lp "$I/sys/usr/lib/hao/aot" 2>/dev/null); m2=$(stat -f %Lp "$I/sys/private/var/db/hao" 2>/dev/null)
[ $rc = 0 ] && [ "$m1" = 755 ] && [ "$m2" = 755 ] && pass "I hao-install.py: usr/lib/hao/aot $m1, private/var/db/hao $m2" \
    || bad "I hao-install.py: rc=$rc modes $m1 $m2 $(tail -2 "$I/log")"
rm -rf "$I/sys/private/var/db/hao" "$I/sys/usr/lib/hao/install.json"; ln -s /tmp "$I/sys/private/var/db/hao"
python3 -I "$repo/tools/hao-install.py" "$I/sys" "$I/pay" >"$I/log2" 2>&1; rc=$?
[ $rc = 1 ] && grep -q "private/var/db/hao (exists and is not a directory)" "$I/log2" && pass "I2 hao-install.py: symlinked private/var/db/hao is an error" \
    || bad "I2 hao-install.py symlink: rc=$rc $(tail -2 "$I/log2")"

# --- J: switches are a startup snapshot: a guest's setenv("HAO_NO_CHAIN")
# does not turn chaining off (before the snapshot: ~20,000,000 blocks)
clang -arch arm64e -O2 -o "$OUT/optsnap.arm64e" "$repo/tests/guest/optsnap.c" && codesign -s - -f "$OUT/optsnap.arm64e" 2>/dev/null
for m in "" --interp; do
    timeout -s KILL 120 "$HAO" --stats $m "$OUT/optsnap.arm64e" x >"$OUT/j.out" 2>"$OUT/j.err"; rc=$?
    b=$(sed -n 's/^hao: blocks=\([0-9]*\).*/\1/p' "$OUT/j.err" | head -1)
    if [ $rc = 0 ] && [ "$(cat "$OUT/j.out")" = 35000000 ] && { [ -n "$m" ] || [ "${b:-999999999}" -lt 1000 ]; }; then
        pass "J${m:+ $m} guest setenv(HAO_NO_CHAIN) mid-run: output 35000000$([ -z "$m" ] && echo ", blocks=$b")"
    else bad "J$m snapshot: rc=$rc out=$(cat "$OUT/j.out") blocks=$b"; fi
done
# no switch is read with getenv outside the snapshot and the hand-offs
g=$(grep -n 'getenv(' "$repo"/src/*.c | grep -vE '^[^:]*/(trust|process_identity)\.c:|getenv\("HAO_(EXEC|EXEC_FLAGS|SELF|FLAGS)"\)')
[ -z "$g" ] && pass "J2 getenv only in the snapshot and the HAO_EXEC/HAO_SELF/HAO_FLAGS hand-offs" || bad "J2 stray getenv: $g"

# --- K: the guest image's descriptor is closed after loading
clang -arch arm64e -O1 -o "$OUT/openfds.arm64e" "$repo/tests/guest/openfds.c" && codesign -s - -f "$OUT/openfds.arm64e" 2>/dev/null
k=$(timeout -s KILL "$T" "$HAO" "$OUT/openfds.arm64e" 2>&1 | tail -1)
[ "$k" = "0 descriptors on the program file" ] && pass "K guest image fd closed after load ($k)" || bad "K open image fd: $k"

# --- L: the load-time harness (tests/aot/loadtime.c) builds and loads the valid cache
clang -arch x86_64 -O1 -I"$repo/include" -o "$OUT/loadtime" "$here/loadtime.c" -L"$B" -lhao -Wl,-rpath,"$B" && codesign -s - -f "$OUT/loadtime" 2>/dev/null
l=$(HAO_CACHE=$C timeout -s KILL "$T" "$OUT/loadtime" "$G" 5 2>/dev/null | tail -1)
case $l in *": 5/5 loaded, min "*) pass "L loadtime: ${l#*: }" ;; *) bad "L loadtime: $l" ;; esac

# --- F: the policy unit test
clang -arch x86_64 -O1 -I"$repo/include" -o "$OUT/policy" "$here/policy.c" "$B/libhaocore.a" && codesign -s - -f "$OUT/policy" 2>/dev/null
mkdir -p "$OUT/ut" && chmod 755 "$OUT/ut"
if timeout -s KILL 120 "$OUT/policy" "$OUT/ut" >"$OUT/policy.log"; then pass "F $(tail -1 "$OUT/policy.log" | sed 's/^PASS //'); $(grep '^fuzz' "$OUT/policy.log")"
else bad "F policy: $(grep FAIL "$OUT/policy.log" | head -5)"; fi

# --- L: dependency paths. In a set-ID process (HAO_DISTRUST_ENV=2 stands in
# for the credentials) dyld expands no @executable_path, no @loader_path from
# the main executable's LC_RPATH and no relative path; the launch then ends
# as a missing library does (dyld's message, SIGABRT). An absolute LC_RPATH
# and @loader_path in a load command still load. Ordinary and guarded
# processes load all of them. Each program returns 42 (35 from its library).
L=$OUT/dep
mkdir -p "$L/lib"
printf 'int dep_value(void) { return 35; }\n' > "$L/dep.c"
printf 'int dep_value(void);\nint main(void) { return dep_value() + 7; }\n' > "$L/main.c"
lok=1
for n in "dep @rpath/libdep.dylib" "rel lib/librel.dylib" "ld @loader_path/lib/libld.dylib" "ex @executable_path/lib/libex.dylib"; do
    set -- $n
    clang -arch arm64e -O1 -dynamiclib -install_name "$2" -o "$L/lib/lib$1.dylib" "$L/dep.c" && codesign -s - -f "$L/lib/lib$1.dylib" 2>/dev/null || lok=0
done
# program, its library, its LC_RPATH ("" = none), set-ID result, dyld's reason
lcase() {
    local name=$1 lib=$2 rpath=$3 want=$4 reason=$5 rc t
    clang -arch arm64e -O1 -o "$L/$name" "$L/main.c" "$L/lib/$lib" ${rpath:+-Wl,-rpath,"$rpath"} && codesign -s - -f "$L/$name" 2>/dev/null || lok=0
    for t in 0 1 2; do
        (cd "$L" && env HAO_DISTRUST_ENV=$t timeout -s KILL "$T" "$HAO" "./$name" >/dev/null 2>"$L/$name.$t.err"); rc=$?
        if [ $t != 2 ] || [ "$want" = 42 ]; then
            [ $rc = 42 ] && pass "L $name tier $t: loads, rc 42" || bad "L $name tier $t: rc=$rc $(tail -2 "$L/$name.$t.err")"
        else
            [ $rc = 134 ] && grep -q "Library not loaded" "$L/$name.$t.err" && grep -q "Reason: tried: .*($reason)" "$L/$name.$t.err" \
                && pass "L $name set-ID tier: not loaded ($reason), rc 134" || bad "L $name set-ID tier: rc=$rc $(tail -2 "$L/$name.$t.err")"
        fi
    done
}
lcase rp_exe libdep.dylib @executable_path/lib 134 "security policy does not allow @ path expansion"
lcase rp_ldr libdep.dylib @loader_path/lib 134 "security policy does not allow @ path expansion"
lcase rp_rel libdep.dylib lib 134 "relative path not allowed in hardened program"
lcase rp_abs libdep.dylib "$L/lib" 42 ""
lcase in_rel librel.dylib "" 134 "relative path not allowed in hardened program"
lcase in_exe libex.dylib "" 134 "security policy does not allow @ path expansion"
lcase in_ldr libld.dylib "" 42 ""
[ $lok = 1 ] || bad "L build"

# --- M: the diagnostics log. Its lock is a 0600 file in the log directory,
# not the directory: another process holding an flock on the directory (any
# user can take one on a 0755 directory) neither delays the start nor loses
# the log. The log is 0600, and one an earlier hao left 0644 becomes 0600.
M=$OUT/diag
mkdir -p "$M" && chmod 755 "$M"
env HAO_DIAG_DIR="$M" timeout -s KILL "$T" "$HAO" "$G" >/dev/null 2>&1; rc=$?
m1=$(stat -f %Lp "$M/console.log" 2>/dev/null); m2=$(stat -f %Lp "$M/.lock" 2>/dev/null)
[ $rc = 7 ] && [ "$m1" = 600 ] && [ "$m2" = 600 ] && pass "M log and lock are 0600" || bad "M modes: rc=$rc console.log $m1 .lock $m2"
chmod 644 "$M/console.log"
m=$(python3 -I - "$HAO" "$G" "$M" "$T" <<'EOF'
import fcntl, os, subprocess, sys, time
hao, guest, d, t = sys.argv[1:]
fd = os.open(d, os.O_RDONLY)
fcntl.flock(fd, fcntl.LOCK_EX)
runs = []
for k in range(5):
    t0 = time.monotonic()
    r = subprocess.run(['timeout', '-s', 'KILL', t, hao, guest], env={'HAO_DIAG_DIR': d, 'PATH': '/usr/bin:/bin'}, capture_output=True)
    runs.append((time.monotonic() - t0, r.returncode))
mode = os.stat(d + '/console.log').st_mode & 0o7777
print('%s %o %.0f' % ('ok' if all(rc == 7 for _, rc in runs) else 'rc', mode, 1000 * min(s for s, _ in runs)))
EOF
)
set -- $m
# the old lock waited 200 ms for the directory's flock and then opened no log (so no mode repair)
[ "$1" = ok ] && [ "$2" = 600 ] && [ "$3" -lt 150 ] && pass "M directory flock held elsewhere: log opened (0644 -> 0600), start ${3} ms" \
    || bad "M directory flock held: $m"

exit $fail
