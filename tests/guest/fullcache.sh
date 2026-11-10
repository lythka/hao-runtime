#!/bin/sh
# The pc a guest SIGSEGV/SIGBUS handler sees once hao's code cache is full
# (tests/guest/fullcache.c). With HAO_JIT_CACHE_MB=1 the fill (FILL_N
# generated functions, about 900 KB of translated code) exhausts the cache,
# and hao_jit_run_block interprets every new block with a64_step; the
# faulting instruction must still be the one the handler sees, as natively
# and in --interp. Cases: a load from a PROT_NONE page and a store to a
# read-only one, before and after the fill, one fault per process. Each runs
# native arm64, then hao JIT, --interp and JIT with the 1 MB cache (also
# under HAO_CODE_ALIAS=1); stdout and the exit status must match native.
#   OUT=dir HAO=/abs/build/hao [T=60] [FILL_N=4000] tests/guest/fullcache.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-fullcache)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
N=${FILL_N:-4000}
python3 - "$N" > "$OUT/fullcache_fill.c" <<'EOF' || exit 2
import sys
n = int(sys.argv[1])
for i in range(n):
    print(f'__attribute__((noinline)) long g{i}(long x) {{ x = x * {i + 3} + {i}; if (x & {1 << (i % 13)}) x ^= {i * 7}; '
          f'else x += {i * 5}; return (x >> 3) * {i % 17 + 1}; }}')
print('long fill(long x) {')
for i in range(n):
    print(f'    x = g{i}(x);')
print('    return x;\n}')
EOF
for a in arm64e arm64; do
    clang -arch $a -O1 -o "$OUT/fullcache.$a" "$here/fullcache.c" "$OUT/fullcache_fill.c" || exit 2
    codesign -s - -f "$OUT/fullcache.$a" 2>/dev/null
done
fail=0
for c in "load nofill" "load fill" "store fill"; do
    k=$(echo "$c" | tr ' ' -)
    timeout -s KILL "$T" "$OUT/fullcache.arm64" $c > "$OUT/$k.native.out" 2>&1
    nrc=$?
    for v in jit interp full fullalias; do
        case $v in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        full) set -- env HAO_NO_AOT=1 HAO_JIT_CACHE_MB=1 "$HAO" --stats ;;
        fullalias) set -- env HAO_NO_AOT=1 HAO_JIT_CACHE_MB=1 HAO_CODE_ALIAS=1 "$HAO" --stats ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/fullcache.arm64e" $c > "$OUT/$k.$v.out" 2> "$OUT/$k.$v.err"
        rc=$?
        note=""
        case $v-$k in full*-*-fill) grep -q "code cache full" "$OUT/$k.$v.err" || { note=" (the cache did not fill: raise FILL_N)"; rc=x; } ;; esac
        if [ "$rc" = "$nrc" ] && cmp -s "$OUT/$k.native.out" "$OUT/$k.$v.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf '%-12s %-10s native rc=%s hao rc=%s %s%s\n' "$k" "$v" "$nrc" "$rc" "$r" "$note"
        [ $r = SAME ] || diff "$OUT/$k.native.out" "$OUT/$k.$v.out" | sed 's/^/    /'
    done
done
[ $fail = 0 ] && echo "fullcache: PASS" || echo "fullcache: FAIL"
exit $fail
