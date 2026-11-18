#!/bin/sh
# A SAME row of the Swift value walk's facts applies only to the native type it names (tests/host/svidentity.swift):
# under fixture rows the runtime's metadata of each type finds its row ("native ... 1"); a heap copy of that
# metadata, the same name and layout, finds none ("forged ... 0"). A TEST_SWITCHES build's libhao.
#   OUT=dir LIB=build-ts tests/host/svidentity.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
LIB=$(cd "${LIB:-$repo/build-ts}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svidentity)}
mkdir -p "$OUT"
xcrun swiftc -O -target x86_64-apple-macos27.0 -o "$OUT/svidentity" "$here/svidentity.swift" || exit 2
codesign -s - -f "$OUT/svidentity" 2>/dev/null
timeout -s KILL 60 "$OUT/svidentity" "$LIB/libhao.dylib" >"$OUT/out" 2>"$OUT/err"; rc=$?
cat "$OUT/out"
n=$(grep -c '^native .* 1$' "$OUT/out"); f=$(grep -c '^forged .* 0$' "$OUT/out")
if [ $rc = 0 ] && [ "$n" = 19 ] && [ "$f" = 18 ] && ! grep -qE '^native .* 0$|^forged .* 1$' "$OUT/out"
then echo "svidentity: PASS"; exit 0; fi
echo "svidentity: FAIL (rc=$rc, $n native rows, $f forged refused)"; exit 1
