#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
fixture="$root/tests/guest/debugger-snapshot"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for arch in arm64 x86_64; do
    clang -arch "$arch" -Wall -Werror "$fixture/target.c" -o "$out/$arch"
    codesign -fs - "$out/$arch" 2>/dev/null
done
python3 "$fixture/session.py" "$out/arm64" > "$out/native"
python3 "$fixture/session.py" "${HAO:-$root/build/hao}" "$out/arm64" > "$out/translated"
diff -u "$out/native" "$out/translated"
python3 "$fixture/session.py" --continuous "${HAO:-$root/build/hao}" "$out/arm64"
if FIXTURE_FAIL_FINAL=1 python3 "$fixture/session.py" --continuous "${HAO:-$root/build/hao}" "$out/arm64" > "$out/nonzero" 2>&1; then
    echo "failed target reported clean exit" >&2
    exit 1
fi
grep -q 'target failed after final resume: 23' "$out/nonzero"
FIXTURE_CORRUPT_PC=1 python3 "$fixture/session.py" --invalid-pc "${HAO:-$root/build/hao}" "$out/arm64"
python3 "$fixture/session.py" --stale-ack "${HAO:-$root/build/hao}" "$out/arm64"
python3 "$fixture/session.py" --negative "$out/x86_64"
FIXTURE_WRONG_MAPPING=1 python3 "$fixture/session.py" --invalid-pc "${HAO:-$root/build/hao}" "$out/arm64"
FIXTURE_BAD_VERSION=1 python3 "$fixture/session.py" --invalid-version "${HAO:-$root/build/hao}" "$out/arm64"
FIXTURE_TRUNCATE=1 python3 "$fixture/session.py" --truncated "${HAO:-$root/build/hao}" "$out/arm64"
python3 - "${HAO:-$root/build/hao}" "$out/arm64" <<'PYTHON'
import subprocess, sys
p = subprocess.run(sys.argv[1:], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, timeout=10)
if p.returncode != 5:
    raise SystemExit("missing controller connection was not refused")
PYTHON
