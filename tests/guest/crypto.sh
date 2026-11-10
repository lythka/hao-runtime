#!/bin/sh
# Build tests/guest/crypto.c for arm64e (guest) and arm64 (reference); run
# arm64 natively and arm64e under hao (JIT, --interp, with an AOT cache) and
# compare stdout and the exit status. The program runs every FEAT_AES,
# FEAT_PMULL, FEAT_SHA1/SHA256/SHA512 and FEAT_SHA3 instruction on fixed
# vectors and in known-answer computations (AES-128, SHA-1/256/512,
# Keccak-f[1600]); a known answer that fails prints WRONG on either side.
#   OUT=dir HAO=build/hao [T=60] [ROUNDS=16] tests/guest/crypto.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-crypto}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
ROUNDS=${ROUNDS:-16}
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -Wall -o "$OUT/crypto.$a" "$here/crypto.c"
    codesign -s - -f "$OUT/crypto.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/crypto.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/crypto.arm64" "$ROUNDS" >"$OUT/native.out" 2>"$OUT/native.err"
nrc=$?
echo "crypto native rc=$nrc ($(grep -c . "$OUT/native.out") lines, $(grep -c ' ok$' "$OUT/native.out") known answers ok, $(grep -c WRONG "$OUT/native.out") WRONG)"
fail=0
[ $nrc = 0 ] && ! grep -q WRONG "$OUT/native.out" || fail=1
for v in jit interp aot; do
    case $v in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/crypto.arm64e" "$ROUNDS" >"$OUT/$v.out" 2>"$OUT/$v.err"
    rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$v.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf 'crypto %-6s rc=%-3s %s (%s lines, %s differ from native)\n' "$v" "$rc" "$r" \
        "$(grep -c . "$OUT/$v.out")" "$(diff "$OUT/native.out" "$OUT/$v.out" | grep -c '^[<>]')"
    [ $r = SAME ] || head -3 "$OUT/$v.err" | sed 's/^/    /'
done
exit $fail
