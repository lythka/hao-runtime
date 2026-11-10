#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
B=$(cd "${B:-$repo/build}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-dscselhoist)}
mkdir -p "$OUT"
clang -arch x86_64 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$repo/include" \
    "$here/dscselhoist.c" -L"$B" -lhao -Wl,-rpath,"$B" -lobjc -o "$OUT/dscselhoist"
codesign -s - -f "$OUT/dscselhoist" 2>/dev/null
timeout -s KILL 30 "$OUT/dscselhoist"
