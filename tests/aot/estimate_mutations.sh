#!/bin/sh
# Independently corrupt exact estimate table rounding and exponent parity.
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-estimate-mutations)}
B=${B:-$repo/build}
T=${T:-240}
mkdir -p "$OUT"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
python3.14 - "$OUT" <<'PY'
from pathlib import Path
import sys
s = Path('src/jit.c').read_text()
mutations = {
    'estimate-rounding': ('((1u << 19) / (2 * a + 1) + 1) / 2', '((1u << 19) / (2 * a + 1)) / 2'),
    'estimate-parity': ('XSHR, 0, RCX, 1); /* odd exponent:', 'XSHR, 0, RCX, 0); /* odd exponent:'),
}
Path(sys.argv[1], 'jit-control.c').write_text(s)
for name, (old, new) in mutations.items():
    assert s.count(old) == 1, (name, s.count(old))
    Path(sys.argv[1], 'jit-' + name + '.c').write_text(s.replace(old, new))
PY
for mutation in control estimate-rounding estimate-parity; do
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        -Da64_exec_helper=jit_vector_helper -c "$OUT/jit-$mutation.c" -o "$OUT/jit-$mutation.o"
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        tests/aot/jit_vector.c "$OUT/jit-$mutation.o" "$B/stubs_oracle.o" "$B/libhaocore.a" -o "$OUT/$mutation"
    codesign -s - -f "$OUT/$mutation" 2>/dev/null
    set +e
    HAO_VECTOR_FAIL_FAST=1 timeout -s KILL "$T" "$OUT/$mutation" >"$OUT/$mutation.log" 2>&1
    rc=$?
    set -e
    if [ "$mutation" = control ]; then
        if [ "$rc" != 0 ] || ! grep -q ', 0 mismatches$' "$OUT/$mutation.log"; then
            echo "FAIL: unmutated control (rc=$rc)"; exit 1
        fi
        echo "PASS: unmutated control"; continue
    fi
    if [ "$rc" != 1 ] || ! grep -q '^MISMATCH ' "$OUT/$mutation.log"; then
        echo "FAIL: $mutation not caught (rc=$rc)"; exit 1
    fi
    echo "PASS: $mutation detected"; tail -1 "$OUT/$mutation.log"
done
