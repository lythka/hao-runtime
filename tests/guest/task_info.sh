#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang -arch arm64 -Wall -Werror "$root/tests/guest/task_info.c" -o "$out/guest"
codesign -fs - "$out/guest" 2>/dev/null
"$out/guest" > "$out/native"
"${HAO:-$root/build/hao}" "$out/guest" > "$out/translated"
diff -u "$out/native" "$out/translated"
if grep -q 'guard=0' "$out/native"; then exit 1; fi
grep -q '^sdk flavor=18 cap=10 kr=0 count=10 guard=1' "$out/native"
grep -q '^legacy-exact flavor=5 cap=8 kr=0 count=8 guard=1' "$out/native"
grep -q '^legacy-small flavor=5 cap=7 kr=4 count=7 guard=1' "$out/native"
