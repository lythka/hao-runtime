#!/bin/sh
# The vector differential test must reject independently broken AES
# translations. Mutate private source copies, never the checkout or runtime.
# OUT=dir [T=120] tests/aot/aes_mutations.sh
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-aes-mutations)}
B=${B:-$repo/build}
T=${T:-240}
mkdir -p "$OUT"
cp tests/aot/jit_vector.c "$OUT/vector.c"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
python3 - "$OUT" <<'PY'
from pathlib import Path
import sys
s = Path('src/jit.c').read_text()
mutations = {
    'missing-round-key': ('vld(t, 0, rd); vld(t, 1, rn); P(0xEF, 0, 1);          /* ARM AddRoundKey */',
                         'vld(t, 0, rd); vld(t, 1, rn); /* deliberately omit AddRoundKey */'),
}
Path(sys.argv[1], 'jit-control.c').write_text(s)
for name, (old, new) in mutations.items():
    assert s.count(old) == 1, (name, s.count(old))
    Path(sys.argv[1], 'jit-' + name + '.c').write_text(s.replace(old, new))
PY
for mutation in control missing-round-key; do
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        -Da64_exec_helper=jit_vector_helper -c "$OUT/jit-$mutation.c" -o "$OUT/jit-$mutation.o"
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        -Itests/aot "$OUT/vector.c" "$OUT/jit-$mutation.o" "$B/stubs_oracle.o" "$B/libhaocore.a" -o "$OUT/$mutation"
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
        echo "FAIL: $mutation was not caught by a result/state or admission mismatch (rc=$rc)"
        exit 1
    fi
    echo "PASS: $mutation detected"
    tail -1 "$OUT/$mutation.log"
done
