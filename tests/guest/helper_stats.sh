#!/bin/sh
# Dynamic helper classification in JIT and AOT; no counting without --stats.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-helper-stats)}
HAO=${HAO:-$repo/build/hao}
mkdir -p "$OUT/aot"; chmod 700 "$OUT/aot"
clang -arch arm64 -O1 -o "$OUT/guest" "$here/helper_stats.c"
codesign -s - -f "$OUT/guest" 2>/dev/null
timeout -s KILL 60 "$HAO" --aot --cache "$OUT/aot" "$OUT/guest" >"$OUT/build.out" 2>&1
for mode in jit aot; do
    case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    for n in 0 1000; do
        timeout -s KILL 60 "$@" --stats "$OUT/guest" "$n" >"$OUT/$mode.$n.out" 2>"$OUT/$mode.$n.err"
    done
    timeout -s KILL 60 "$@" "$OUT/guest" 1000 >"$OUT/$mode.quiet.out" 2>"$OUT/$mode.quiet.err"
    ! grep -q 'helper group' "$OUT/$mode.quiet.err"
done
python3.14 - "$OUT" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1])
def counts(mode, n):
    text = (p / f'{mode}.{n}.err').read_text()
    groups = {(g,k): int(v) for g,k,v in re.findall(r'helper group (\S+) kind=(\S+) calls=(\d+)', text)}
    return groups, int(re.search(r'helper_calls=(\d+)', text)[1])
for mode in ('jit', 'aot'):
    a, at = counts(mode, 0); b, bt = counts(mode, 1000)
    assert bt - at == 4000, (mode, at, bt)
    for key in [('simd-misc', 'untranslated'), ('fp-vector', 'declined'),
                ('fp-vector', 'untranslated'), ('integer-simd', 'untranslated')]:
        assert b.get(key,0) - a.get(key,0) == 1000, (mode, key, a, b)
    print(f'PASS {mode}: 3000 untranslated, 1000 declined, 1000 admitted')
PY
