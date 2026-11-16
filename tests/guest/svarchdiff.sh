#!/bin/sh
# A guest value holding Swift.Float16, a frozen type declared differently by architecture
# (src/archdiff_gen.inc: x86_64 libswiftCore's Float16 has no stored field and size 0), must be refused
# by the walker (src/swiftvalue/Recipe.c holds_archdiff): a guest struct with a Float16 field, Float16
# itself and an Optional nil of the struct; the same struct with an Int16 there is walked (one patch).
# The value lines (tests/guest/svarchdiff.swift; generic ones go through Float16's native metadata, which
# hao gives a 2-byte POD's witnesses) must equal native arm64 under JIT, --interp and AOT. Where this
# host's libswiftCore is not the build the table describes, Float16 is unlisted and only the control is checked.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/svarchdiff.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svarchdiff)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
LIB=$(cd "$(dirname "$HAO")" && pwd)/libhao.dylib
for a in arm64e arm64; do
    xcrun swiftc -O -target $a-apple-macos27.0 -o "$OUT/svarchdiff.$a" "$here/svarchdiff.swift" 2>"$OUT/build.$a.err" ||
        { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svarchdiff.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svarchdiff.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svarchdiff: aot generation failed"
timeout -s KILL "$T" "$OUT/svarchdiff.arm64" >"$OUT/native" 2>/dev/null; nrc=$?
fail=0
[ $nrc = 0 ] || fail=1
grep '^value ' "$OUT/native" >"$OUT/native.value"
[ -s "$OUT/native.value" ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/svarchdiff.arm64e" "$LIB" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    grep '^value ' "$OUT/$mode.out" >"$OUT/$mode.value"
    if [ $rc = 0 ] && cmp -s "$OUT/native.value" "$OUT/$mode.value"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s value lines %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.value" "$OUT/$mode.value" | sed 's/^/    /' | head -16; tail -3 "$OUT/$mode.err"; }
done
cp "$OUT/jit.out" "$OUT/out"; cp "$OUT/jit.err" "$OUT/err"
cat "$OUT/out"
grep -qx 'hao walk Short 1' "$OUT/out" || fail=1
grep -q '^hao Float16 metadata .* listed \(yes\|no\)$' "$OUT/out" || { echo "no table lookup in this libhao"; fail=1; }
if grep -q '^hao Float16 metadata .* listed yes$' "$OUT/out"; then
    for l in 'hao walk Half refused svarchdiff.Half' 'hao walk Float16 refused Swift.Float16' \
             'hao walk Optional<Half> refused Swift.Optional<svarchdiff.Half>'; do
        grep -qxF "$l" "$OUT/out" || { echo "missing: $l"; fail=1; }
    done
else
    echo "note: Swift.Float16 unlisted (this host's libswiftCore is not the table's build); refusals not checked"
fi
[ $fail = 0 ] && echo "svarchdiff: PASS" || echo "svarchdiff: FAIL"
exit $fail
