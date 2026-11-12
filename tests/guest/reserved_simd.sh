#!/bin/sh
# Compare reserved selectors and unsigned estimates with native arm64.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-reserved_simd)}
HAO=${HAO:-$repo/build/hao}
T=${T:-180}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -march=armv8.5-a+fp16 -O1 -fno-builtin -o "$OUT/reserved_simd.$a" "$here/reserved_simd.c"
    codesign -s - -f "$OUT/reserved_simd.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/reserved_simd.arm64e" >/dev/null 2>&1
set +e
timeout -s KILL "$T" "$OUT/reserved_simd.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp helpers aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/reserved_simd.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
    # The runtime reports HAO_EXIT_UNDEF by aborting, rather than delivering
    # guest SIGILL. Check its diagnostic, not an arbitrary nonzero exit.
    k=0
    for word in 0e605820 5e202820 2e31b820 0e011c20 5e010c20 0f086420 5f080420 0ec21820 5f422020; do
        timeout -s KILL "$T" "$OUT/reserved_simd.arm64" "$k" >"$OUT/native-$k.out" 2>&1
        if [ $? != 0 ] || ! grep -q "selector $k: SIGILL" "$OUT/native-$k.out"; then fail=1; fi
        timeout -s KILL "$T" "$@" "$OUT/reserved_simd.arm64e" "$k" >"$OUT/$mode-$k.out" 2>"$OUT/$mode-$k.err"
        rc=$?
        if [ "$rc" != 134 ] || ! grep -q "undefined instruction.*insn=$word" "$OUT/$mode-$k.err"; then
            echo "FAIL $mode reserved selector $k rc=$rc"; fail=1
        fi
        k=$((k+1))
    done
    echo "$mode: checked nine reserved selectors"
done
[ $fail = 0 ] && echo "reserved_simd: PASS" || echo "reserved_simd: FAIL"
exit $fail
