#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-near-map)}
mkdir -p "$OUT"
clang -arch x86_64 -O2 -g -Wall -Wextra -o "$OUT/near-map" "$here/near-map.c"
codesign -s - -f "$OUT/near-map" 2>/dev/null
timeout -s KILL 30 "$OUT/near-map"
