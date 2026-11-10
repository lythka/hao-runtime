#!/bin/sh
# Build tests/guest/bitfields.m (+ native helper dylib bfhelper.m) for arm64e
# (guest) and arm64 (reference); run arm64 natively and arm64e under hao
# (JIT and --interp, and optionally a baseline hao, $HAO_BASE). Prints
# SAME/DIFF per run against native arm64 and every canary report that is
# not intact; exits 1 if any hao run differs.
#   OUT=dir HAO=build/hao [HAO_BASE=/path/to/old/hao] [T=60] tests/guest/bitfields.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-bitfields)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -fobjc-arc -I"$here" -framework Foundation \
    -install_name "$OUT/libbfhelper.dylib" -o "$OUT/libbfhelper.dylib" "$here/bfhelper.m"
codesign -s - -f "$OUT/libbfhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lbfhelper \
        -o "$OUT/bitfields.$a" "$here/bitfields.m"
    codesign -s - -f "$OUT/bitfields.$a" 2>/dev/null
done
set +e
timeout -s KILL "$T" "$OUT/bitfields.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
echo "bitfields native rc=$? ($(grep -c . "$OUT/native.out") lines)"
fail=0
run() {  # label cmd...
    rl=$1; shift
    timeout -s KILL "$T" "$@" >"$OUT/$rl.out" 2>"$OUT/$rl.err"
    rc=$?
    if cmp -s "$OUT/native.out" "$OUT/$rl.out"; then rr=SAME; else rr=DIFF; fi
    printf 'bitfields %-7s rc=%-3s %s\n' "$rl" "$rc" "$rr"
    grep -E 'OVERRUN' "$OUT/$rl.out" | sed 's/^/    /'
    [ "$rl" = base ] || [ "$rr" = SAME ] || fail=1
}
run jit    "$HAO" "$OUT/bitfields.arm64e"
run interp "$HAO" --interp "$OUT/bitfields.arm64e"
if [ -n "${HAO_BASE:-}" ]; then
    run base "$HAO_BASE" "$OUT/bitfields.arm64e"
fi
exit $fail
