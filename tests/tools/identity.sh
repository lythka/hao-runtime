#!/bin/bash
# The stub carries the program's load-time identity by rule (hao-embed, tools/stub_identity_gate.py RULES):
# every load command and header field of the arm64e slice that dyld, the kernel, AMFI or the frameworks read,
# except a reasoned deny-list.
#   OUT=dir [B=build] [EMBED=tools/hao-embed] [X86_CACHE=dir] tests/tools/identity.sh
# Fixture (tests/tools/identity_main.c + identity_helper.c, command line only, libSystem only): an arm64e
# program at macOS 14.0/sdk 15.0 with LC_SOURCE_VERSION 1234.5.6, an @executable_path/plug LC_RPATH,
# LC_DYLD_ENVIRONMENT DYLD_LIBRARY_PATH=@executable_path/envlib, LC_MAIN stacksize 16 MB, libSystem linked at
# 1300.0.0, signed with the kill and hard CS flags; "restrict" also has a __RESTRICT,__restrict section. Cases:
#  - a native helper (the x86_64 slice under hao) reports the main executable as native code reads it: header
#    walk (source version, rpaths, environment, stacksize, __PAGEZERO, extra segments, MH flags),
#    dyld_get_program_sdk_version/minos, NSVersionOfLinkTimeLibrary("System"), the process's CS kill/hard
#    flags (csops), the main thread's stack size, a dlopen of an @rpath name (the main executable's rpath) and
#    of a leaf name (LC_DYLD_ENVIRONMENT); the embedded program under JIT and --interp prints what the same
#    source built for native arm64 prints;
#  - DYLD_INSERT_LIBRARIES: inserted into the plain program, ignored for the __RESTRICT one, as natively;
#  - the gate: stub_identity_gate.py finds no DIFF in the embedded files and in a --stubs payload (with the
#    x86_64 cache: the linked dylibs too), and finds the differences in a fat file whose stub is plain hao-stub;
#  - fail closed: a program with a load command type the rule table does not name is refused.
# Exit 1 if any case failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools-identity)}
B=${B:-$repo/build}
EMBED=${EMBED:-$repo/tools/hao-embed}
GATE=${GATE:-$repo/tools/stub_identity_gate.py}
fails=0
X86=${X86_CACHE:-}
for c in /System/Volumes/Preboot/Cryptexes/Rosetta/System/Library/dyld /System/Library/dyld; do
    [ -n "$X86" ] || { [ -f "$c/dyld_shared_cache_x86_64" ] && X86=$c; }
done
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT" || exit 2

# libraries: fat (x86_64 for the stub, arm64 natively, arm64e for completeness)
mkdir -p plug envlib
lib() {  # ROLE OUT INSTALL-NAME
    clang -arch x86_64 -arch arm64 -arch arm64e -mmacosx-version-min=14.0 -dynamiclib -O1 -D$1 -install_name "$3" -o "$2" "$here/identity_helper.c" &&
        codesign -s - -f "$2" 2>/dev/null || exit 2
}
lib ROLE_HELPER libidhelp.dylib "$OUT/libidhelp.dylib"   # absolute: a restricted process expands no @ paths
lib ROLE_PLUG plug/libidplug.dylib @rpath/libidplug.dylib
lib ROLE_ENV envlib/libidenv.dylib libidenv.dylib
lib ROLE_INSERT libidinsert.dylib "$OUT/libidinsert.dylib"

# LC_SOURCE_VERSION (ld writes 0) -> 1234.5.6, libSystem's LC_LOAD_DYLIB current_version -> 1300.0.0
# (NSVersionOfLinkTimeLibrary reads the main executable's)
set_fields() {
    python3 - "$1" <<'PY'
import struct, sys
p = sys.argv[1]
mo = bytearray(open(p, "rb").read())
ncmds, off, done = struct.unpack_from("<I", mo, 16)[0], 32, set()
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", mo, off)
    if cmd == 0x2A:
        struct.pack_into("<Q", mo, off + 8, (1234 << 40) | (5 << 30) | (6 << 20))
        done.add("source")
    if cmd == 0xC:
        o = struct.unpack_from("<I", mo, off + 8)[0]
        if mo[off + o:off + size].split(b"\0")[0] == b"/usr/lib/libSystem.B.dylib":
            struct.pack_into("<I", mo, off + 16, 1300 << 16)
            done.add("libSystem")
    off += size
assert done == {"source", "libSystem"}, done
open(p, "wb").write(mo)
PY
}
build() {  # NAME EXTRA-LD-ARGS...
    local n=$1; shift
    for arch in arm64 arm64e; do
        clang -arch $arch -O1 -o $n.$arch.tmp "$here/identity_main.c" libidhelp.dylib -Wl,-rpath,@executable_path/plug \
            -Wl,-dyld_env,DYLD_LIBRARY_PATH=@executable_path/envlib -Wl,-stack_size,0x1000000 \
            -mmacosx-version-min=14.0 -Wl,-platform_version,macos,14.0,15.0 -Wl,-no_adhoc_codesign "$@" &&
            mv $n.$arch.tmp $n.$arch && set_fields $n.$arch &&
            codesign -s - -f -o kill,hard $n.$arch 2>/dev/null || exit 2
    done
}
build idprog
build restrict -Wl,-sectcreate,__RESTRICT,__restrict,/dev/null

# Under Rosetta (this development Mac) an x86_64 process's main thread stack is 8 MB whatever its LC_MAIN
# stacksize says (measured: a native x86_64 build of the fixture with -stack_size 16M reports 0x7fc000), so the
# line is compared only on x86_64 hardware; LC_MAIN's stacksize itself is compared everywhere.
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ] || [ "$(uname -m)" = arm64 ]; then
    unrosetta() { grep -v '^main thread stack'; }
    echo "NOTE Rosetta: the main thread stack size line is not compared"
else unrosetta() { cat; }; fi
for n in idprog restrict; do
    timeout -s KILL 60 ./$n.arm64 >$n.native 2>$n.native.err
    timeout -s KILL 60 env DYLD_INSERT_LIBRARIES=$OUT/libidinsert.dylib ./$n.arm64 >$n.ins.native 2>&1
    if ! python3 "$EMBED" --host-test --stub "$B/hao-stub" -o $n.emb $n.arm64e >embed-$n.log 2>&1; then
        bad "$n: hao-embed: $(tr '\n' ' ' <embed-$n.log)"; continue
    fi
    for mode in jit interp; do
        env=(); [ $mode = interp ] && env=(HAO_INTERP=1)
        timeout -s KILL 120 env ${env[@]+"${env[@]}"} arch -x86_64 ./$n.emb >$n.$mode 2>$n.$mode.err
        if [ -s $n.native ] && diff <(unrosetta <$n.native) <(unrosetta <$n.$mode) >$n.$mode.diff; then
            pass "$n $mode: native code sees the main executable as natively ($(wc -l <$n.native | tr -d ' ') lines)"
        else bad "$n $mode: differs from native arm64: $(tr '\n' ' ' <$n.$mode.diff) $(head -c 300 $n.$mode.err)"; fi
    done
    # arch is a platform binary: dyld drops DYLD_* from its environment, so give it to the child with -e
    timeout -s KILL 120 arch -x86_64 -e DYLD_INSERT_LIBRARIES=$OUT/libidinsert.dylib ./$n.emb >$n.ins.hao 2>$n.ins.err
    if [ -s $n.ins.native ] && diff <(grep -c '^inserted' $n.ins.native) <(grep -c '^inserted' $n.ins.hao) >/dev/null; then
        pass "$n: DYLD_INSERT_LIBRARIES as natively ($(grep -q '^inserted' $n.ins.native && echo inserted || echo ignored))"
    else bad "$n: DYLD_INSERT_LIBRARIES: native '$(grep -c '^inserted' $n.ins.native)' embedded '$(grep -c '^inserted' $n.ins.hao)'"; fi
done
# what native arm64 shows (the fixture is what it says)
want="source_version 1234.5.6.0.0;LC_MAIN stacksize 0x1000000;rpath @executable_path/plug;"
want+="dyld_environment DYLD_LIBRARY_PATH=@executable_path/envlib;sdk 15.0 minos 14.0;libSystem link version 1300.0.0;cs kill=1 hard=1;"
want+="main thread stack 0x1000000;dlopen @rpath/libidplug.dylib: ok;dlopen libidenv.dylib (DYLD_LIBRARY_PATH): ok"
got=$(grep -E '^(source_version|rpath|dyld_env|LC_MAIN|sdk|libSystem|cs |main thread|dlopen)' idprog.native | tr '\n' ';')
[ "$got" = "$want;" ] && pass "idprog: native arm64 has the identity the fixture sets" || bad "idprog native: '$got'"
grep -q "^segment __RESTRICT " restrict.native && grep -q "^dlopen libidenv.dylib (DYLD_LIBRARY_PATH): failed" restrict.native &&
    grep -q "^dlopen @rpath/libidplug.dylib: failed" restrict.native &&
    pass "restrict: native arm64 is restricted (__RESTRICT: LC_DYLD_ENVIRONMENT ignored, no @ path expansion)" || bad "restrict native: $(tr '\n' ';' <restrict.native)"
grep -q '^inserted' idprog.ins.native && ! grep -q '^inserted' restrict.ins.native &&
    pass "native arm64: DYLD_INSERT_LIBRARIES inserted into idprog, ignored for restrict" ||
    bad "native arm64 DYLD_INSERT_LIBRARIES: idprog '$(cat idprog.ins.native | head -1)' restrict '$(head -1 restrict.ins.native)'"

# --- the gate
python3 "$GATE" --host-test idprog.emb restrict.emb >gate.out 2>&1 && pass "gate: embedded files equal to their arm64e slices by rule" ||
    bad "gate: $(grep DIFF gate.out | tr '\n' ' ')"
lipo -create idprog.arm64e "$B/hao-stub" -output plain.emb 2>/dev/null || exit 2
python3 "$GATE" --host-test plain.emb >gate-plain.out 2>&1
rc=$?
for f in source_version rpath dyld_environment main.stacksize build.sdk build.minos uuid sig.kill sig.hard; do
    grep -q "^DIFF	plain.emb	$f	" gate-plain.out || { bad "gate: plain hao-stub's $f not reported"; rc=0; }
done
[ $rc = 1 ] && pass "gate: a plain hao-stub slice differs (exit 1: source_version, rpath, dyld_environment, stacksize, build version, uuid, CS flags)"
mkdir -p root/usr/bin root/usr/bin/plug root/usr/bin/envlib
cp idprog.arm64e root/usr/bin/idprog && cp libidhelp.dylib root/usr/bin/ && cp plug/libidplug.dylib root/usr/bin/plug/
python3 "$EMBED" --stub "$B/hao-stub" --stubs pay --root root --x86-cache "$X86" --scan >pay.log 2>&1 || bad "--stubs: $(cat pay.log)"
python3 "$GATE" --x86-cache "$X86" --stubs pay --root root --out gate-pay >gate-pay.out 2>&1 &&
    grep -q "^| dylibs | 1 | 0 |" gate-pay/summary.md && pass "gate --stubs: payload equal by rule, linked dylibs = native_deps" ||
    bad "gate --stubs: $(cat gate-pay.out | tr '\n' ' ')"

# --- fail closed: a load command type no rule names (LC_SOURCE_VERSION's type changed to 0x7e)
cp idprog.arm64e unknown.arm64e
python3 - unknown.arm64e <<'PY'
import struct, sys
p = sys.argv[1]
mo = bytearray(open(p, "rb").read())
ncmds, off = struct.unpack_from("<I", mo, 16)[0], 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", mo, off)
    if cmd == 0x2A:
        struct.pack_into("<I", mo, off, 0x7E)
    off += size
open(p, "wb").write(mo)
PY
codesign -s - -f unknown.arm64e 2>/dev/null || exit 2
python3 "$EMBED" --host-test --stub "$B/hao-stub" -o unknown.emb unknown.arm64e >unknown.log 2>&1
grep -q "^fail.*load command 0x7e has no stub identity rule" unknown.log && [ ! -e unknown.emb ] &&
    pass "fail closed: an unnamed load command type is refused" || bad "fail closed: $(cat unknown.log)"

[ $fails = 0 ] && echo "identity: all cases passed" || echo "identity: $fails failed"
[ $fails = 0 ]
