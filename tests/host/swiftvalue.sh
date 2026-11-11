#!/bin/sh
# hao's Swift value walk over native x86_64 values (tests/host/swiftvalue.swift): Optional<AnyObject>
# beside an affected enum is walked (nil, NSObject, Swift object, tagged NSNumber), as Optional<NSObject>
# is; a nested Optional<Optional<AnyObject>> stays refused (its second nil is an extra inhabitant whose
# encoding differs between arm64 and x86_64). Every line must match the expected list.
#   OUT=dir LIB=build tests/host/swiftvalue.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
LIB=$(cd "${LIB:-$repo/build}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftvalue)}
mkdir -p "$OUT"
xcrun swiftc -O -target x86_64-apple-macos27.0 -o "$OUT/swiftvalue" "$here/swiftvalue.swift"
codesign -s - -f "$OUT/swiftvalue" 2>/dev/null
timeout -s KILL 60 "$OUT/swiftvalue" "$LIB/libhao.dylib" >"$OUT/out" 2>"$OUT/err"
cat "$OUT/out"
fail=0
for e in payload "inner nil" "outer nil"; do
    for c in "AnyObject? nil" "AnyObject? NSObject" "AnyObject? Swift object" "AnyObject? tagged NSNumber" "NSObject? (control)"; do
        grep -q "^$c, $e walk [0-9]" "$OUT/out" || { echo "FAIL $c, $e: not walked"; fail=1; }
    done
    grep -q "^AnyObject?? (stays refused), $e refused" "$OUT/out" || { echo "FAIL nested Optional, $e: not refused"; fail=1; }
done
[ $fail = 0 ] && echo "swiftvalue: PASS" || echo "swiftvalue: FAIL"
exit $fail
