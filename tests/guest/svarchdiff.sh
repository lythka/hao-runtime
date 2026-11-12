#!/bin/sh
# A guest value holding Swift.Float16, a frozen type declared differently by architecture
# (src/archdiff_gen.inc: x86_64 libswiftCore's Float16 has no stored field and size 0), must be refused
# by the walker (src/swiftvalue/Recipe.c holds_archdiff): a guest struct with a Float16 field, Float16
# itself and an Optional nil of the struct; the same struct with an Int16 there is walked (one patch).
# The value lines (tests/guest/svarchdiff.swift) must equal native arm64. Where this host's libswiftCore
# is not the build the table describes, Float16 is unlisted and only the control is checked.
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
timeout -s KILL "$T" "$OUT/svarchdiff.arm64" >"$OUT/native" 2>/dev/null; nrc=$?
timeout -s KILL "$T" "$HAO" "$OUT/svarchdiff.arm64e" "$LIB" >"$OUT/out" 2>"$OUT/err"; rc=$?
cat "$OUT/out"
fail=0
[ $nrc = 0 ] && [ $rc = 0 ] || fail=1
grep '^value ' "$OUT/native" >"$OUT/native.value"; grep '^value ' "$OUT/out" >"$OUT/hao.value"
[ -s "$OUT/native.value" ] && cmp -s "$OUT/native.value" "$OUT/hao.value" || { echo "value lines differ from native arm64"; fail=1; }
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
[ $fail = 0 ] && echo "svarchdiff: PASS" || { tail -3 "$OUT/err"; echo "svarchdiff: FAIL"; }
exit $fail
