#!/bin/sh
# Host-only, unprivileged regression for receipt generation and default forwarding.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(CDPATH= cd -- "${1:-$repo/build}" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/hao-spawn-receipt.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
cc -arch arm64 -arch x86_64 "$repo/tests/spawn-pref/child.c" -o "$out/original"
codesign -f -s - "$out/original"
if "$repo/tools/hao-spawn-receipt" "$out/original" > "$out/unprepared"; then
    echo "unprepared executable received a receipt" >&2
    exit 1
fi
test ! -s "$out/unprepared"
"$repo/tools/hao-embed" --stub "$build/hao-stub" --translate -o "$out/selected" "$out/original"
"$repo/tools/hao-spawn-receipt" "$out/selected" > "$out/receipt"
python3 - "$out" <<'PY'
from pathlib import Path
import hashlib
import sys
root = Path(sys.argv[1])
receipt = (root / 'receipt').read_bytes()
assert len(receipt) == 82 and receipt[:8] == b'haospwn1'
assert receipt[8:40] == hashlib.sha256((root / 'selected').read_bytes()).digest()
assert receipt[40] and receipt[61] and receipt[41:61] != receipt[62:]
PY
"$build/test_spawn_shape"
