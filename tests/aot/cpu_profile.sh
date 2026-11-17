#!/bin/sh
# The translation profile of AES-NI and PCLMULQDQ lowerings, CRC32C, the
# generator identity and the profile used for fault-recovery re-emission
# (tests/aot/cpu_profile.c).   OUT=dir [B=build] tests/aot/cpu_profile.sh
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-cpu-profile)}
B=${B:-$repo/build}
mkdir -p "$OUT"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
    tests/aot/cpu_profile.c "$B/stubs_oracle.o" "$B/libhaocore.a" -o "$OUT/cpu-profile"
codesign -s - -f "$OUT/cpu-profile" 2>/dev/null
timeout -s KILL 90 "$OUT/cpu-profile"
