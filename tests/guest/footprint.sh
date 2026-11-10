#!/bin/sh
# hao's per-process memory overhead (against jetsam per-process limits):
# tests/guest/footprint.m's footprint under
# hao (JIT, --interp, AOT) minus the same program's natively on arm64, as
# footprint(1) counts it (dirty + swapped: what jetsam compares with a
# process's limit). Rosetta's own categories ("Rosetta *": translating hao
# itself, native code on x86_64 hardware) are left out.
#   c        printf only (the minimal guest)
#   objc     Foundation, a dispatch queue, a run-loop turn
#   spread   N guest functions, one per page (as functions of many images
#            are), each handed to native code: a thunk, a translation and a
#            bridge plan each. A hashed table spreads such entries over a
#            page each, so a fixed table sized for the worst case is dirtied
#            whole (libhao's __DATA held 13 MB of them before).
#   bigaot   the minimal guest with BIG_FNS functions of code it never
#            runs, under AOT: the cache is installed whole (about twice its
#            size), so its overhead is printed, not judged ("info"), and
#   limited  the same under HAO_AOT_LIMIT_KB=LIMIT_KB, standing in for a
#            jetsam limit (modelmanagerd: 15 MB, a 19 MB cache): hao must
#            skip the cache (one "not loaded under the process's memory
#            limit" line), run the program translated, and stay in
#            BUDGET_SPREAD (its 6000 function pointers get thunks at load).
# Each judged case fails when its overhead exceeds its budget (KB):
#   BUDGET_C (2048), BUDGET_OBJC (2048), BUDGET_SPREAD (3072)
# Why: a 15 MB InactiveHard jetsam limit (modelmanagerd, findmylocateagent
# on 27.0) minus such a daemon's native footprint (findmylocateagent: 13.7 MB
# natively on this 27.2 host, KernelEventAgent 2.8 MB) leaves 1 to 12 MB;
# hao's code-independent share must stay a small part of that. Measured on
# the 27.2 development Mac under Rosetta: about 1.1 MB for c and objc, about
# 1 MB of it page tables; spread 1.4 MB (6.5 MB before the growable tables).
# Prints one line per case and PASS/FAIL; exit 0 if all pass, 1 otherwise.
#   OUT=dir HAO=/abs/build/hao [FP_TIMEOUT=60] [N=1024] [BIG_FNS=6000] [LIMIT_KB=16384] [MODES="jit interp aot"] tests/guest/footprint.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-footprint)}
HAO=${HAO:-$here/../../build/hao}
N=${N:-1024}
BIG_FNS=${BIG_FNS:-6000}
LIMIT_KB=${LIMIT_KB:-16384}
MODES=${MODES:-jit interp aot}
BUDGET_C=${BUDGET_C:-2048}
BUDGET_OBJC=${BUDGET_OBJC:-2048}
BUDGET_SPREAD=${BUDGET_SPREAD:-3072}
T=${T:-90}
mkdir -p "$OUT"
case $HAO in /*) ;; *) echo "HAO must be an absolute path"; exit 2 ;; esac

# spread: N functions, one per 4 KB page; big: BIG_FNS functions of 48 statements
python3 - "$OUT" "$N" "$BIG_FNS" <<'EOF' || exit 2
import sys
out, n, big = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def table(f, n):
    f.write("static int (*const fp_fns[FP_NFNS])(const void *, const void *) = {\n")
    f.write(",\n".join("fp_cmp%d" % k for k in range(n)))
    f.write("};\n")
with open(out + "/spread_fns.h", "w") as f:
    f.write("#define FP_NFNS %d\n" % n)
    for k in range(n):
        f.write("__attribute__((aligned(4096), noinline)) static int fp_cmp%d(const void *a, const void *b)"
                " { return *(const int *)a - *(const int *)b + %d * 0; }\n" % (k, k))
    table(f, n)
with open(out + "/big_fns.h", "w") as f:
    f.write("#define FP_NFNS %d\n" % big)
    for k in range(big):
        body = " ".join("x = (x * %d) ^ (x >> %d) ^ v[%d];" % (2 * j + 3 + k % 7, j % 13 + 1, j % 4) for j in range(48))
        f.write("__attribute__((noinline)) static int fp_cmp%d(const void *a, const void *b)"
                " { const int *v = a; int x = *(const int *)b; %s return x; }\n" % (k, body))
    table(f, big)
EOF
for a in arm64e arm64; do
    for g in fp:-UFP_FNS fpspread:-DFP_FNS=\"$OUT/spread_fns.h\" fpbig:-DFP_FNS=\"$OUT/big_fns.h\"; do
        clang -arch $a -O1 -fobjc-arc -framework Foundation "${g#*:}" -o "$OUT/${g%%:*}.$a" "$here/footprint.m" ||
            { echo "build of ${g%%:*}.$a failed"; exit 2; }
        codesign -s - -f "$OUT/${g%%:*}.$a" 2>/dev/null
    done
done
case " $MODES " in *" aot "*)
    rm -rf "$OUT/aotc"; mkdir -p "$OUT/aotc"; chmod 700 "$OUT/aotc"
    for g in fp fpspread fpbig; do
        HAO_CACHE=$OUT/aotc timeout -s KILL 300 "$HAO" --aot "$OUT/$g.arm64e" >"$OUT/aot-$g.log" 2>&1 ||
            { echo "hao --aot $g failed: $(tail -3 "$OUT/aot-$g.log")"; exit 2; }
    done ;;
esac

# measure NAME CMD...: the process's footprint in KB without Rosetta's categories,
# read once it has done its work ("footprint_kb=" printed): "KB PAGETABLE_KB TOTAL_KB"
measure() {
    name=$1; shift
    rm -f "$OUT/$name.json"
    FP_HOLD=1 timeout -s KILL "$T" "$@" >"$OUT/$name.out" 2>"$OUT/$name.err" &
    tpid=$!
    i=0
    while ! grep -q '^footprint_kb=' "$OUT/$name.out" 2>/dev/null; do
        i=$((i + 1)); [ $i -gt $((T * 10)) ] && break
        kill -0 $tpid 2>/dev/null || break
        sleep 0.1
    done
    pid=$(pgrep -P $tpid | head -1); [ -n "$pid" ] || pid=$tpid
    # footprint(1) can hang (the 27.2 development Mac: on any process, /bin/sleep too):
    # bounded, a hang is a missing measurement, which fails the case
    grep -q '^footprint_kb=' "$OUT/$name.out" &&
        timeout -s KILL "${FP_TIMEOUT:-60}" footprint -j "$OUT/$name.json" "$pid" >/dev/null 2>&1
    kill -9 "$pid" $tpid 2>/dev/null
    wait $tpid 2>/dev/null
    python3 - "$OUT/$name.json" <<'EOF'
import json, sys
try:
    p = json.load(open(sys.argv[1]))["processes"][0]
except Exception:
    print("- - -"); sys.exit(0)
c = p["categories"]
kb = lambda v: (v["dirty"] + v["swapped"]) // 1024
own = sum(kb(v) for k, v in c.items() if not k.startswith("Rosetta"))
pt = kb(c["page table"]) if "page table" in c else 0
print(own, pt, p["footprint"] // 1024)
EOF
}

fails=0
printf '%-8s %-7s %9s %9s %9s %9s %7s  %s\n' case route native_kb hao_kb overhead pt_delta budget result
for cs in c objc spread bigaot limited; do
    modes=$MODES
    case $cs in
    c) g=fp; args=c; budget=$BUDGET_C ;;
    objc) g=fp; args=objc; budget=$BUDGET_OBJC ;;
    spread) g=fpspread; args="thunks $N"; budget=$BUDGET_SPREAD ;;
    bigaot) g=fpbig; args=c; budget=info; modes=aot ;;
    limited) g=fpbig; args=c; budget=$BUDGET_SPREAD; modes=aot ;;
    esac
    case " $MODES " in *" aot "*) ;; *) [ "$modes" = aot ] && continue ;; esac
    set -- $(measure "$cs-native" "$OUT/$g.arm64" $args)
    nat=$1; natpt=$2
    for m in $modes; do
        case $cs:$m in
        limited:aot) set -- $(measure "$cs-$m" env HAO_CACHE="$OUT/aotc" HAO_AOT_LIMIT_KB="$LIMIT_KB" "$HAO" "$OUT/$g.arm64e" $args) ;;
        *:jit) set -- $(measure "$cs-$m" env HAO_NO_AOT=1 "$HAO" "$OUT/$g.arm64e" $args) ;;
        *:interp) set -- $(measure "$cs-$m" env HAO_NO_AOT=1 "$HAO" --interp "$OUT/$g.arm64e" $args) ;;
        bigaot:aot) set -- $(measure "$cs-$m" env HAO_CACHE="$OUT/aotc" HAO_VERBOSE=1 "$HAO" "$OUT/$g.arm64e" $args) ;;
        *:aot) set -- $(measure "$cs-$m" env HAO_CACHE="$OUT/aotc" "$HAO" "$OUT/$g.arm64e" $args) ;;
        esac
        h=$1; hpt=$2
        want=$(sed -n 1p "$OUT/$cs-native.out")
        got=$(sed -n 1p "$OUT/$cs-$m.out")
        skipped=$(grep -c "not loaded under the process's memory limit" "$OUT/$cs-$m.err")
        loaded=$(grep -c "AOT cache .* loaded" "$OUT/$cs-$m.err")
        if [ "$nat" = - ] || [ "$h" = - ]; then r="FAIL (no measurement)"; ov=-; ptd=-
        else
            ov=$((h - nat)); ptd=$((hpt - natpt))
            if [ "$want" != "$got" ]; then r="FAIL (output '$got', native '$want')"
            elif [ $cs = limited ] && [ "$skipped" != 1 ]; then r="FAIL (the cache was not skipped under the limit)"
            elif [ $cs = bigaot ] && [ "$loaded" = 0 ]; then r="FAIL (the cache was not loaded)"
            elif [ "$budget" = info ]; then r="info (cache $(ls -lS "$OUT/aotc" | awk 'NR == 2 { print int($5 / 1024) }') KB)"
            elif [ $ov -gt "$budget" ]; then r=FAIL
            else r=PASS; fi
        fi
        case $r in PASS|info*) ;; *) fails=$((fails + 1)) ;; esac
        printf '%-8s %-7s %9s %9s %9s %9s %7s  %s\n' "$cs" "$m" "$nat" "$h" "$ov" "$ptd" "$budget" "$r"
    done
done
[ $fails = 0 ] && echo "footprint: all PASS" || echo "footprint: $fails FAIL"
[ $fails = 0 ]
