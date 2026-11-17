#!/bin/sh
# Diagnostics written from signal handlers (src/runtime.c hao_diag_write, hao_diag_write_fatal): no formatting
# or allocation on the way, also when a fatal report opens a log that was waiting for its directory or a
# line reaches the log's cap (tests/host/diagsig.c; tests/host/diagsig_interpose.c counts the calls).
#   OUT=dir LIB=build tests/host/diagsig.sh     exit 0 if all checks pass
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
LIB=$(cd "${LIB:-$repo/build}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-sigdiag)}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
cc="clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1"
$cc -dynamiclib -o "$OUT/diagsig_interpose.dylib" "$here/diagsig_interpose.c" || { echo "build failed"; exit 1; }
$cc -I"$repo/include" -o "$OUT/diagsig" "$here/diagsig.c" -L"$LIB" -lhao -Wl,-rpath,"$LIB" || { echo "build failed"; exit 1; }
codesign -s - -f "$OUT/diagsig" "$OUT/diagsig_interpose.dylib" 2>/dev/null
rm -rf "$OUT/dir"; mkdir -p "$OUT/dir"
timeout -s KILL 60 "$OUT/diagsig" "$OUT/dir" "$OUT/diagsig_interpose.dylib" 2>"$OUT/diagsig.err"
rc=$?
[ $rc = 0 ] && echo "diagsig: PASS" || { sed 's/^/  stderr: /' "$OUT/diagsig.err" | tail -5; echo "diagsig: FAIL"; }
exit $rc
