#!/bin/sh
# Native generic code (tests/guest/settledresult_lib.swift, a universal library-evolution Swift library) reads a
# guest conformance's associated-type getter through its witness table: the requirement's interface unit and
# thunk library (settledresult_thunks.c) give hao the getter's reverse recipe and a Copyable result, so each
# call's result (UInt32, Int, Double: types settled as a whole) is admitted by the Swift value walk
# (src/swiftresult.c). The program is arm64 natively and arm64e under hao (JIT, --interp, AOT); every line
# must equal native arm64 with no refusal, and HAO_BRIDGE_CHECK=1 must report all 27 results resolved (the
# coverage control). Before the fix each such result was refused and the run aborted.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/settledresult.sh
set -u
export HAO_NATIVE_APP_LIBS=1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-settledresult)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT/if" "$OUT/aot"
chmod 755 "$OUT" "$OUT/if"
chmod 700 "$OUT/aot"
set -e
for t in x86_64 arm64 arm64e; do
    xcrun swiftc -target $t-apple-macos27.0 -O -enable-library-evolution -emit-library -module-name SRLib \
        -emit-module -emit-module-path "$OUT/mod-$t/SRLib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$OUT/libSRLib.dylib" -o "$OUT/lib-$t.dylib" "$here/settledresult_lib.swift"
done
lipo -create "$OUT/lib-x86_64.dylib" "$OUT/lib-arm64.dylib" "$OUT/lib-arm64e.dylib" -output "$OUT/libSRLib.dylib"
codesign -s - -f "$OUT/libSRLib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    xcrun swiftc -target $t-apple-macos27.0 -O -I "$OUT/mod-$t" -L"$OUT" -lSRLib -o "$OUT/settledresult.$t" \
        "$here/settledresult.swift"
    codesign -s - -f "$OUT/settledresult.$t" 2>/dev/null
done
xcrun clang -arch x86_64 -arch arm64e -dynamiclib -o "$OUT/if/sr.thunks" "$here/settledresult_thunks.c"
codesign -s - -f "$OUT/if/sr.thunks" 2>/dev/null
req=$(nm -arch x86_64 "$OUT/libSRLib.dylib" | grep -oE '_\$s5SRLib6SourceP5value[A-Za-z0-9_]*vgTj' | sort -u)
printf '%s\tTYPED\t-\t-\t-\t-\t-\tksrvalue\n' "$req" >"$OUT/unit.tsv"
U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$OUT/libSRLib.dylib")
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/if/$U" --uuid "$U" "$OUT/unit.tsv"
chmod 644 "$OUT/if/$U" "$OUT/if/sr.thunks"
HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/settledresult.arm64e" >/dev/null 2>&1
set +e
fail=0
timeout -s KILL "$T" "$OUT/settledresult.arm64" >"$OUT/native.out" 2>"$OUT/native.err"; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$@" "$OUT/settledresult.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value boundary refused' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16; grep -a 'Swift value\|walk' "$OUT/$mode.err" | head -4 | cut -c1-300; }
done
HAO_IF_DIR="$OUT/if" HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 timeout -s KILL "$T" "$HAO" --stats "$OUT/settledresult.arm64e" \
    >"$OUT/check.out" 2>"$OUT/check.err"
n=$(grep -c '^hao: associated result Value metadata=' "$OUT/check.err")
grep -q 'associated results: bound=[0-9]* checked=27 refused=0' "$OUT/check.err" && [ "$n" = 27 ] &&
    cmp -s "$OUT/native.out" "$OUT/check.out" && r=SAME || { r="DIFF (associated results: $n of 27)"; fail=1; }
echo "check     $r"
[ $fail = 0 ] && echo "settledresult: PASS" || echo "settledresult: FAIL"
exit $fail
