#!/bin/sh
# Packed stack arguments of a native Swift function that end with a CGFloat (plan kind 'K',
# hao/bridge.h), against native arm64. Past x7 and d7, Apple arm64 puts each argument at its
# natural size and alignment (a CGFloat?'s tag byte, then a Bool in the next byte) and the last
# CGFloat after them; x86_64 swiftcc gives each its own 8-byte slot, the CGFloat too. hao's
# default copy of 8-byte guest stack words gives the callee CGFloat? values built from a tag and
# a Bool, and the trailing CGFloat from beyond the arguments. The record lists, per SysV slot,
# the arm64 offset and width; the CGFloat is a plain 8-byte slot. With the unit every hao run
# (JIT, --interp, helpers-only, AOT) must print what native arm64 prints; without it the values
# must arrive wrong (the control).
#   OUT=dir HAO=build/hao [T=60] tests/guest/stackpackfp.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-stackpackfp)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT/if" "$OUT/aot"
chmod 755 "$OUT" "$OUT/if"
chmod 700 "$OUT/aot"
set -e
for t in x86_64 arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -enable-library-evolution \
        -module-name SPFLib -emit-module -emit-module-path "$OUT/mod-$t/SPFLib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$OUT/libSPFLib.dylib" -o "$OUT/lib-$t.dylib" "$here/stackpackfp_lib.swift"
done
lipo -create "$OUT/lib-x86_64.dylib" "$OUT/lib-arm64.dylib" "$OUT/lib-arm64e.dylib" -output "$OUT/libSPFLib.dylib"
codesign -s - -f "$OUT/libSPFLib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -I "$OUT/mod-$t" -L"$OUT" -lSPFLib -o "$OUT/stackpackfp.$t" "$here/stackpackfp.swift"
    codesign -s - -f "$OUT/stackpackfp.$t" 2>/dev/null
done
sym=$(nm -arch x86_64 "$OUT/libSPFLib.dylib" | grep -oE '_\$s6SPFLib7MetricsV10fontSource[A-Za-z0-9_]*tcfC' | sort -u)
[ -n "$sym" ]
# x6, x7: SysV slots 0, 1. Then the two objects, five (CGFloat payload, tag) pairs with the two Bools
# after the first two tags, and the CGFloat: what the arm64 caller stores at sp+0 .. sp+0x67.
enc=0.8,8.8,16.8,24.1,25.1,32.8,40.1,41.1,48.8,56.1,64.8,72.1,80.8,88.1,96.8
printf '%s\tTYPED\tK\t-\t-\t%s\t-\n' "$sym" "$enc" >"$OUT/unit.tsv"
U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$OUT/libSPFLib.dylib")
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/if/$U" --uuid "$U" "$OUT/unit.tsv"
chmod 644 "$OUT/if/$U"
HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/stackpackfp.arm64e" >/dev/null 2>&1
set +e
fail=0
timeout -s KILL "$T" "$OUT/stackpackfp.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] && [ "$(wc -l <"$OUT/native.out" | tr -d ' ')" = 4 ] || fail=1
for mode in jit interp helpers aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$@" "$OUT/stackpackfp.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
# Without the unit the first case's nil cornerRadius must arrive as a value (the control).
HAO_NO_IFUNITS=1 HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/stackpackfp.arm64e" >"$OUT/nounit.out" 2>"$OUT/nounit.err"
if head -1 "$OUT/nounit.out" | grep -q ' corner=nil '; then echo "control: the packed arguments arrived intact without the unit"; fail=1
else echo "control: without the unit $(head -1 "$OUT/nounit.out" | grep -oE 'corner=[^ ]*') (native corner=nil)"; fi
[ $fail = 0 ] && echo "stackpackfp: PASS" || echo "stackpackfp: FAIL"
exit $fail
