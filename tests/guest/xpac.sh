#!/bin/sh
# tests/guest/xpac.c for arm64e (guest) and arm64 (reference): XPAC* on x30
# in functions hao enters with its return sentinel, and XPAC* results. Runs
# natively and under hao (JIT, --interp, AOT); stdout and rc must match.
#   OUT=dir HAO=/abs/build/hao [T=30] tests/guest/xpac.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-xpac)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -o "$OUT/xpac.$a" "$here/xpac.c"
    codesign -s - -f "$OUT/xpac.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL "$T" "$HAO" --aot "$OUT/xpac.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/xpac.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
nrc=$?
fail=0
[ $nrc = 0 ] || { echo "native run failed rc=$nrc"; fail=1; }
for v in jit interp aot; do
    case $v in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/xpac.arm64e" >"$OUT/$v.out" 2>"$OUT/$v.err"
    rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$v.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-6s native rc=%-3s hao rc=%-3s %s\n' "$v" "$nrc" "$rc" "$r"
done
exit $fail
