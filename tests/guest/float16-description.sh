#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${OUT:-"$root/build/float16-description"}
hao=${HAO:-"$root/build/hao"}
mkdir -p "$out"
xcrun swiftc -O -target arm64-apple-macos14.0 "$root/tests/guest/float16-description.swift" -o "$out/descriptions"
codesign -f -s - "$out/descriptions" 2>/dev/null
"$out/descriptions" > "$out/native"
test "$(wc -l < "$out/native")" -eq 65536
for mode in jit interp; do
    command=("$hao")
    if [[ $mode == interp ]]; then command+=(--interp); fi
    HAO_NO_AOT=1 "${command[@]}" "$out/descriptions" > "$out/$mode"
    cmp "$out/native" "$out/$mode"
done
