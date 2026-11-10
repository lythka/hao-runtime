#!/bin/sh
# hao in a process without executable JIT memory (HAO_NO_JIT_MEMORY=1 makes
# the code-cache mmap fail, as a sandbox that denies dynamic-code-generation
# does): the guest runs in the interpreter, native->guest calls take the
# NX-fault path, and struct signatures of callbacks come from the side table.
# Builds the guest tests for arm64e (guest) and arm64 (reference), runs arm64
# natively and arm64e under hao in that mode, and checks for each program:
#   - stdout and exit status identical to native arm64;
#   - exactly one "no executable memory" diagnostic, and no JIT statistics;
#   - no block entry point was patched (hao-blocks patch counters all 0).
# abi (ObjC methods with struct arguments called by native NSInvocation) and
# blocks2 (blocks with struct signatures invoked by native code) must also
# take NX faults and report signatures noted for the callback path. An AOT
# cache that loads under the JIT must be skipped in this mode.
#   OUT=dir HAO=build/hao [T=60] tests/guest/nojit.sh     exit 1 on any failure
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-nojit)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fblocks -I"$here" \
    -install_name "$OUT/libblockhelper.dylib" -o "$OUT/libblockhelper.dylib" "$here/blockhelper.c"
codesign -s - -f "$OUT/libblockhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    for p in hello features sorts blocks; do clang -arch $a -O1 -o "$OUT/$p.$a" "$here/$p.c"; done
    clang -arch $a -O1 -framework Foundation -o "$OUT/abi.$a" "$here/abi.m"
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lblockhelper \
        -o "$OUT/blocks2.$a" "$here/blocks2.m"
    for p in hello features sorts blocks abi blocks2; do codesign -s - -f "$OUT/$p.$a" 2>/dev/null; done
done
set +e
fail=0
bad() { echo "FAIL $1: $2"; fail=1; }
for p in hello features sorts abi blocks blocks2; do
    timeout -s KILL "$T" "$OUT/$p.arm64" >"$OUT/$p.native.out" 2>"$OUT/$p.native.err"
    nrc=$?
    timeout -s KILL "$T" env HAO_NO_JIT_MEMORY=1 HAO_NX_STATS=1 "$HAO" --stats "$OUT/$p.arm64e" \
        >"$OUT/$p.nojit.out" 2>"$OUT/$p.nojit.err"
    rc=$?
    nx=$(sed -n 's/^hao: nx faults: \([0-9]*\) native->guest.*/\1/p' "$OUT/$p.nojit.err" | head -1)
    printf '%-8s native rc=%-3s nojit rc=%-3s nx=%s\n' "$p" "$nrc" "$rc" "${nx:-?}"
    [ "$rc" = "$nrc" ] || bad $p "exit status $rc, native $nrc"
    cmp -s "$OUT/$p.native.out" "$OUT/$p.nojit.out" || bad $p "stdout differs from native arm64"
    [ "$(grep -c 'no executable memory for translated code' "$OUT/$p.nojit.err")" = 1 ] || bad $p "diagnostic not printed once"
    grep -q '^hao-jit:' "$OUT/$p.nojit.err" && bad $p "JIT statistics present (code was translated)"
    grep -q '^hao-blocks: global=[0-9]* invoke_patches=0 helper_patches=0 byref_patches=0' "$OUT/$p.nojit.err" ||
        bad $p "block entry points were patched"
    case $p in abi|blocks2) [ "${nx:-0}" -gt 0 ] || bad $p "no NX-fault callbacks";; esac
done
# Their struct-signature callbacks go through the guest signature table
# (without it both programs crash or print wrong values).
for p in abi blocks2; do
    timeout -s KILL "$T" env HAO_NO_JIT_MEMORY=1 HAO_VERBOSE=2 "$HAO" "$OUT/$p.arm64e" >"$OUT/$p.v2.out" 2>"$OUT/$p.v2.err"
    noted=$(grep -c 'signature noted for the callback path' "$OUT/$p.v2.err")
    printf '%-8s signatures noted for the callback path: %s\n' "$p" "$noted"
    [ "$noted" -gt 0 ] || bad $p "no signature noted"
    cmp -s "$OUT/$p.native.out" "$OUT/$p.v2.out" || bad $p "stdout differs from native arm64 (verbose run)"
done
# An AOT cache that loads under the JIT is skipped when there is no code
# cache to install it in, and the program still runs correctly.
mkdir -p "$OUT/cache"; chmod 755 "$OUT/cache"; rm -f "$OUT/cache/"*
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/cache" "$OUT/sorts.arm64e" >/dev/null 2>&1
timeout -s KILL "$T" env HAO_CACHE="$OUT/cache" HAO_VERBOSE=1 "$HAO" "$OUT/sorts.arm64e" >"$OUT/aot.jit.out" 2>"$OUT/aot.jit.err"
timeout -s KILL "$T" env HAO_CACHE="$OUT/cache" HAO_VERBOSE=1 HAO_NO_JIT_MEMORY=1 "$HAO" "$OUT/sorts.arm64e" \
    >"$OUT/aot.nojit.out" 2>"$OUT/aot.nojit.err"
rc=$?
jl=$(grep -c 'AOT cache .* loaded' "$OUT/aot.jit.err"); nl=$(grep -c 'AOT cache .* loaded' "$OUT/aot.nojit.err")
echo "aot      cache loaded: jit $jl, nojit $nl; nojit rc=$rc"
[ "$jl" = 1 ] || bad aot "control: the cache did not load under the JIT"
[ "$nl" = 0 ] || bad aot "cache loaded without a code cache"
[ $rc = 0 ] && cmp -s "$OUT/sorts.native.out" "$OUT/aot.nojit.out" || bad aot "sorts with a cache and no JIT memory differs from native"
[ $fail = 0 ] && echo "nojit: PASS" || echo "nojit: FAIL"
exit $fail
