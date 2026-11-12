#!/bin/sh
# The vector differential test must reject independently broken FP32/FP16 FMLA
# translations. Mutate private source copies, never the checkout or runtime.
# OUT=dir [T=120] tests/aot/fmla_mutations.sh
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-fmla-mutations)}
B=${B:-$repo/build}
T=${T:-120}
mkdir -p "$OUT"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
python3 - "$OUT" <<'PY'
from pathlib import Path
import sys
s = Path('src/jit.c').read_text()
mutations = {
    'half-round': ('P(0xFB, 3, 6);                                             /* half rounding increment */', 'P(0xEB, 3, 3);'),
    'half-tiny': ('vconst(t, 6, 1051, false);', 'vconst(t, 6, 1050, false);'),
    'half-index': ('half_idx = BIT(i, 11) << 2 | BIT(i, 21) << 1 | BIT(i, 20);', 'half_idx = BIT(i, 20) << 2 | BIT(i, 21) << 1 | BIT(i, 11);'),
    'half-exact': ('x_sse_rri(&t->b, 0x66, 0xC2, 6, 7, 4);                    /* half exactness: error != 0 */', 'vzero(t, 6);'),
    'half-negation': ('P(0xEF, 8, 0); } /* half FMLS negate */', 'P(0xEB, 8, 8); }'),
    'odd': ('P(0xD4, s, b);', 'P(0xEB, s, s);'),
    'negation': ('P(0xEF, 0, 2);                                           /* negate product exactly */', 'P(0xEB, 0, 0);'),
    'index': ('idx = BIT(i, 11) << 1 | BIT(i, 21);', 'idx = BIT(i, 21) << 1 | BIT(i, 11);'),
}
Path(sys.argv[1], 'jit-control.c').write_text(s)
for name, (old, new) in mutations.items():
    assert s.count(old) == 1, (name, s.count(old))
    Path(sys.argv[1], 'jit-' + name + '.c').write_text(s.replace(old, new))
PY
for mutation in control odd negation index half-round half-tiny half-index half-exact half-negation; do
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        -Da64_exec_helper=jit_vector_helper -c "$OUT/jit-$mutation.c" -o "$OUT/jit-$mutation.o"
    clang -arch x86_64 -march=x86-64-v2 -std=gnu11 -O2 -Iinclude -Isrc \
        tests/aot/jit_vector.c "$OUT/jit-$mutation.o" "$B/stubs_oracle.o" "$B/libhaocore.a" -o "$OUT/$mutation"
    codesign -s - -f "$OUT/$mutation" 2>/dev/null
    set +e
    timeout -s KILL "$T" "$OUT/$mutation" >"$OUT/$mutation.log" 2>&1
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
