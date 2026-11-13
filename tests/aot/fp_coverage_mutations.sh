#!/bin/sh
# Independently corrupt pair order/result admission in private translator copies.
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-fp-coverage-mutations)}
B=${B:-$repo/build}
T=${T:-240}
mkdir -p "$OUT"
make B="$B" "$B/libhaocore.a" "$B/stubs_oracle.o" >/dev/null
python3.14 - "$OUT" <<'PY'
from pathlib import Path
import sys
s = Path('src/jit.c').read_text()
s = s.replace('#include "jit-conversions.inc"', Path('src/jit-conversions.inc').read_text())
mutations = {
    'fabd-sign': ('if (absolute) P(0xDB, 2, mag); /* FABD absolute result */', '/* wrong signed FABD result */'),
    'satadd-qc': ('vset_qc(t, 8); /* saturating add/sub active overflow */', '/* wrong: no saturation flag */'),
    'scalar-fused-sign': ('if (three && BIT(insn, 21)) P(0xEF, 8, 2);', 'if (false) P(0xEF, 8, 2);'),
    'rsqrt-scale': ('P(0x59, 9, 2); if (Q) P(0x59, 13, 2);', '/* deliberately omit reciprocal square-root scale */'),


    'half-subtract': ('P(arithmetic[op], 0, 1); /* exact half arithmetic */', 'P(arithmetic[op == HALF_SUB ? HALF_ADD : op], 0, 1); /* wrong half subtraction */'),
    'conversion-round': ('rounds[mode]); /* roundsd, with PE */', '3); /* wrong conversion rounding */'),

    'fpcr-rounding': ('XSHR, 0, RDX, 8); x_alu_ri(&t->b, XAND, 0, RDX, 1u << 14);', 'XSHR, 0, RDX, 9); x_alu_ri(&t->b, XAND, 0, RDX, 1u << 14);'),
    'fccmp-condition': ('a64_cond_to_xcc(cond) ^ 1) : 0;', 'a64_cond_to_xcc(cond)) : 0;'),
    'division-result': ('size_t exact = jrcxnz32(t); /* packed arithmetic result admission */', 'if (op == VFP_DIV) x_mov_ri(&t->b, RCX, 0); size_t exact = jrcxnz32(t);'),
    'pair-order': ('0, 1, 0x88); /* pair-add even lanes */', '0, 1, 0x82); /* wrong pair-add lanes */'),
}
Path(sys.argv[1], 'jit-control.c').write_text(s)
for name, (old, new) in mutations.items():
    assert s.count(old) == 1, (name, s.count(old))
    Path(sys.argv[1], 'jit-' + name + '.c').write_text(s.replace(old, new))
PY
for mutation in ${MUTATIONS:-control fabd-sign pair-order division-result scalar-fused-sign rsqrt-scale conversion-round half-subtract fpcr-rounding fccmp-condition satadd-qc}; do
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
