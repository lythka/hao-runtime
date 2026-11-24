#!/bin/sh
set -eu
: "${OUT:?}" "${HAO:?}"
cc -arch arm64 -Wall -Wextra tests/guest/ffi_control.c -lffi -o "$OUT/ffi-control"
"$OUT/ffi-control" >"$OUT/native.log" 2>&1
"$HAO" "$OUT/ffi-control" >"$OUT/guest.log" 2>&1
cmp "$OUT/native.log" "$OUT/guest.log"
"$HAO" "$OUT/ffi-control" translated-checks
if "$HAO" "$OUT/ffi-control" data-target >"$OUT/data.log" 2>&1; then exit 1; fi
grep -q 'target is not callable' "$OUT/data.log"
if "$HAO" "$OUT/ffi-control" cross-jump >"$OUT/jump.log" 2>&1; then exit 1; fi
grep -q 'nonlocal exit across native closure' "$OUT/jump.log"
if grep -q 'escaped native closure' "$OUT/jump.log"; then exit 1; fi
if HAO_NO_CLOSURES=1 "$HAO" "$OUT/ffi-control" guest-only >"$OUT/refusal.log" 2>&1; then
    exit 1
fi
grep -q 'no exact guest call entry' "$OUT/refusal.log"
if grep -q 'entered guest sum' "$OUT/refusal.log"; then exit 1; fi
c++ -arch arm64 -Wall -Wextra tests/guest/ffi_unwind.cpp -lffi -o "$OUT/ffi-unwind"
"$OUT/ffi-unwind" >"$OUT/unwind-native.log" 2>&1
grep -q 'caught native-unwound value 17' "$OUT/unwind-native.log"
if "$HAO" "$OUT/ffi-unwind" >"$OUT/unwind-guest.log" 2>&1; then exit 1; fi
grep -q 'nonlocal exit across native closure' "$OUT/unwind-guest.log"
if grep -q 'caught native-unwound' "$OUT/unwind-guest.log"; then exit 1; fi
