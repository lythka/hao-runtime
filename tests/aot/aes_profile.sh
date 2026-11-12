#!/bin/sh
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-aes-profile)}
B=${B:-$repo/build}
mkdir -p "$OUT"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
    tests/aot/aes_profile.c "$B/stubs_oracle.o" "$B/libhaocore.a" -o "$OUT/aes-profile"
codesign -s - -f "$OUT/aes-profile" 2>/dev/null
timeout -s KILL 90 "$OUT/aes-profile"
