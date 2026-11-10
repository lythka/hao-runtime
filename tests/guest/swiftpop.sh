#!/bin/sh
# The swifttailcc callee pop of guest Swift partials that the native executor resumes from a main-queue job
# (src/runtime.c callee_pop, src/swiftcc.c arg_slots; WallpaperAgent's autorelease pool abort).
# Builds tests/guest/swiftpop.swift with -Osize for arm64e (guest) and arm64 (reference), then:
#   - offline (tests/host/swiftslots.c): the partials must call an outlined sp-moving prologue (outlined_sp > 0,
#     else the test lost its coverage) and the scan must give every one of them 0 stack slots (over=0);
#   - arm64 natively, arm64e under hao with the JIT, --interp and an AOT cache, and with HAO_SWIFT_POP=signature
#     (the scan alone) and =frame (the caller frame alone): each run must print what native arm64 prints and exit 0;
#   - HAO_BRIDGE_CHECK=1 with HAO_DIAG_DIR: no "swifttailcc pop" disagreement in the diagnostics log.
# Before the fix every hao run aborted in objc_autoreleasePoolPop ("autorelease pool page ... corrupted").
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/swiftpop.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftpop)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
B=$(cd "$(dirname "$HAO")" && pwd)
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
fail=0
for a in arm64e arm64; do
    xcrun swiftc -Osize -target $a-apple-macos27.0 -o "$OUT/swiftpop.$a" "$here/swiftpop.swift" || { echo "swiftpop: build failed"; exit 1; }
    codesign -s - -f "$OUT/swiftpop.$a" 2>/dev/null
done
rm -f "$OUT/swiftslots" "$OUT/scan.txt"
if [ ! -f "$repo/tests/host/swiftslots.c" ]; then
    echo "SKIP scan: no tests/host/swiftslots.c (the offline scanner is not in this tree)"
else
clang -arch x86_64 -O1 -I"$repo/include" -o "$OUT/swiftslots" "$repo/tests/host/swiftslots.c" -L"$B" -lhao -Wl,-rpath,"$B" 2>"$OUT/swiftslots.err" &&
    codesign -s - -f "$OUT/swiftslots" 2>/dev/null
scan=$( [ -x "$OUT/swiftslots" ] && timeout -s KILL "$T" "$OUT/swiftslots" "$OUT/swiftpop.arm64e" | tee "$OUT/scan.txt" | tail -1)
osp=$(echo "$scan" | sed -n 's/.* outlined_sp=\([0-9]*\).*/\1/p')
over=$(echo "$scan" | sed -n 's/.* over=\([0-9]*\).*/\1/p')
r=OK
[ "${osp:-0}" -gt 0 ] || r="FAIL(no outlined sp-moving prologue: coverage lost)"
[ "${over:-x}" = 0 ] || r="FAIL(over-counted partials: ${over:-?})"
[ -x "$OUT/swiftslots" ] || r="FAIL(tests/host/swiftslots.c does not build against $B)"
[ "$r" = OK ] || fail=1
echo "scan      $r outlined_sp=${osp:-?} over=${over:-?}"
grep -s '^swiftslots: over' "$OUT/scan.txt" | head -4 | sed 's/^/    /'
fi
rm -rf "$OUT/aot" "$OUT/diag"; mkdir -m 700 "$OUT/aot" "$OUT/diag"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/swiftpop.arm64e" >"$OUT/aot.gen" 2>&1 || echo "swiftpop: aot generation failed"
timeout -s KILL "$T" "$OUT/swiftpop.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native    rc=$rc $(cat "$OUT/native.out")"
[ $rc = 0 ] || fail=1
for mode in jit interp aot signature frame check; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    signature) set -- env HAO_NO_AOT=1 HAO_SWIFT_POP=signature "$HAO" ;;
    frame) set -- env HAO_NO_AOT=1 HAO_SWIFT_POP=frame "$HAO" ;;
    check) set -- env HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 HAO_DIAG_DIR="$OUT/diag" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/swiftpop.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    r=SAME
    [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" || r=DIFF
    if [ $mode = check ]; then
        n=$(cat "$OUT/diag/console.log" 2>/dev/null | grep -c 'swifttailcc pop of guest')
        [ "$n" = 0 ] || r="$r(pop disagreements: $n)"
    fi
    [ "$r" = SAME ] || fail=1
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ "$r" = SAME ] || { grep -h 'swifttailcc pop' "$OUT/diag/console.log" 2>/dev/null | head -4
        grep -v '^hao: \(blocks\|threads\|bridge\)\|^hao-\|: bridge: ' "$OUT/$mode.err" | head -6; } | sed 's/^/    /'
done
[ $fail = 0 ] && echo "swiftpop: PASS" || echo "swiftpop: FAIL"
exit $fail
