#!/bin/sh
# Swift's __VaListBuilder for arm64 guest code (tests/guest/valist.swift): withVaList into vsnprintf, a
# list whose storage grows, String(format:) with mixed arguments, an empty list, 2,000 builders made and
# released. The builder's init, append and deinit are inlined into the guest with the
# arm64 layout; hao answers its metadata accessor, va_list() and deallocStorage (src/valist_builder.c).
# Built for arm64 (native reference) and arm64e (guest under the JIT, --interp and AOT); every run must
# print what native prints.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/valist.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-valist)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -o "$OUT/valist.$a" "$here/valist.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/valist.$a" 2>/dev/null
done
# the shape under test: the guest imports the builder's accessor and deallocStorage
nm -u "$OUT/valist.arm64e" | grep -q '__VaListBuilderC14deallocStorage' ||
    { echo "valist: the guest does not import __VaListBuilder.deallocStorage"; exit 2; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/valist.arm64e" >"$OUT/aot.gen" 2>&1 || echo "valist: aot generation failed"
timeout -s KILL "$T" "$OUT/valist.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/valist.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -12; tail -3 "$OUT/$mode.err" | sed 's/^/    err: /'; }
done
[ $fail = 0 ] && echo "valist: PASS" || echo "valist: FAIL"
exit $fail
