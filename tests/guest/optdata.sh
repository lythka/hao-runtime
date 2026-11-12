#!/bin/sh
# Foundation.Data? passed to and returned by a native Swift function (plan kind
# 'D', hao/bridge.h): a universal Swift library typed by an interface unit keyed
# by its x86_64 LC_UUID, against native arm64. A live Data has the same bytes on
# both architectures; the Optional's nil does not, and the arm64 nil read by
# x86_64 code is a present, empty Data. With the unit every hao run (JIT,
# --interp, helpers-only, AOT) must print what native arm64 prints; without it
# the nil arguments must arrive wrong (the case the unit exists for).
#   OUT=dir HAO=build/hao [T=60] tests/guest/optdata.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-optdata)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT/if" "$OUT/aot"
chmod 755 "$OUT" "$OUT/if"
chmod 700 "$OUT/aot"
set -e
for t in x86_64 arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -module-name ODLib \
        -emit-module -emit-module-path "$OUT/mod-$t/ODLib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$OUT/libODLib.dylib" -o "$OUT/lib-$t.dylib" "$here/optdata_lib.swift"
done
lipo -create "$OUT/lib-x86_64.dylib" "$OUT/lib-arm64.dylib" "$OUT/lib-arm64e.dylib" -output "$OUT/libODLib.dylib"
codesign -s - -f "$OUT/libODLib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -I "$OUT/mod-$t" -L"$OUT" -lODLib -o "$OUT/optdata.$t" "$here/optdata.swift"
    codesign -s - -f "$OUT/optdata.$t" 2>/dev/null
done
sym() { nm -arch x86_64 "$OUT/libODLib.dylib" | grep -oE "_\\\$s5ODLib$1[A-Za-z0-9_]*" | sort -u; }
printf '%s\tTYPED\tD\t-\t-\t.---d+\t-\n%s\tTYPED\tD\t-\t-\t.d+-d+\t-\n%s\tTYPED\tD\t-\t-\tR-\t-\n' \
    "$(sym 11odSignature)" "$(sym 5odTwo)" "$(sym 7odMaybe)" >"$OUT/unit.tsv"
U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$OUT/libODLib.dylib")
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/if/$U" --uuid "$U" "$OUT/unit.tsv"
chmod 644 "$OUT/if/$U"
HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/optdata.arm64e" >/dev/null 2>&1
set +e
fail=0
timeout -s KILL "$T" "$OUT/optdata.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] || fail=1
for mode in jit interp helpers aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    helpers) set -- env HAO_NO_AOT=1 HAO_JIT_HELPERS_ONLY=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$@" "$OUT/optdata.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
# Without the unit the nil arguments must arrive as empty Data (the test's control).
HAO_NO_IFUNITS=1 HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/optdata.arm64e" >"$OUT/nounit.out" 2>"$OUT/nounit.err"
if grep -q '^sig nil -1$' "$OUT/nounit.out"; then echo "control: nil arrived intact without the unit"; fail=1
else echo "control: without the unit the nil arrives as $(grep '^sig nil' "$OUT/nounit.out")"; fi
[ $fail = 0 ] && echo "optdata: PASS" || echo "optdata: FAIL"
exit $fail
