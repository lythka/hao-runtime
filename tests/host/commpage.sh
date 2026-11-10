#!/bin/sh
# The arm64 commpage emulation (src/commpage.c), relocated to free addresses
# (tests/host/commpage.c): mapped at startup, mapped again on first use when
# it is gone, a foreign mapping left alone. Exit 0 if all checks pass.
#   OUT=dir [INC=include] tests/host/commpage.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-commpage}
INC=${INC:-$repo/include}
mkdir -p "$OUT"
T=$OUT/test_commpage
clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1 -I"$INC" -DCP_BASE=0x300000004000ULL -DCP_RO=0x300000010000ULL \
    -o "$T" "$here/commpage.c" "$repo/src/commpage.c" || { echo "build failed"; exit 1; }
codesign -s - -f "$T" 2>/dev/null
timeout -s KILL 30 "$T" 2>"$OUT/commpage.err"
rc=$?
cat "$OUT/commpage.err" | sed 's/^/  diag: /'
exit $rc
