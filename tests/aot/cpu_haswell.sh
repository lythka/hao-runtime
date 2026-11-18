#!/bin/bash
# A Haswell-class CPU profile on the development host. Rosetta with
# ROSETTA_ADVERTISE_AVX=1 enumerates AVX, AVX2, FMA, F16C, BMI1/2, MOVBE,
# LZCNT and RDRAND with the AVX state enabled in XCR0; without it, AES and
# PCLMULQDQ only. Checks, with and without the variable:
#   H1 hao's feature word (include/hao/cpufeat.h) sees each profile;
#   H2 the oracle's simd and dp records match native arm64 under the wider one;
#   H3 `hao --aot --cpu=baseline` writes the same bytes under both;
#   H4 a cache whose header needs AVX2 loads under the wider profile and is
#      rejected, by name, under the narrower one (the subset rule).
#   OUT=dir [B=build] [N=20000] [T=60] tests/aot/cpu_haswell.sh
# Prints PASS/FAIL per case; SKIP where hao does not run under Rosetta.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
B=${B:-$repo/build}
HAO=${HAO:-$B/hao}
OUT=${OUT:?set OUT to a scratch directory}
N=${N:-20000}
T=${T:-60}
fail=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
mkdir -p "$OUT" && chmod 755 "$OUT" && OUT=$(cd "$OUT" && pwd -P)
# the probe runs as x86_64: under Rosetta on an Apple silicon host
cat >"$OUT/probe.c" <<'EOF'
#include "hao/cpufeat.h"
#include <stdio.h>
int main(void) { char b[256]; uint64_t f = hao_cpu_host(); printf("%#llx %s\n", (unsigned long long)f, hao_cpu_names(f, b, sizeof b)); return 0; }
EOF
clang -arch x86_64 -I"$repo/include" -o "$OUT/probe" "$OUT/probe.c" "$B/libhaocore.a" && codesign -s - -f "$OUT/probe" 2>/dev/null || exit 1
if [ "$(arch -x86_64 sysctl -n sysctl.proc_translated 2>/dev/null)" != 1 ]; then
    echo "SKIP not under Rosetta: ROSETTA_ADVERTISE_AVX has no effect"; exit 0
fi
narrow=$("$OUT/probe") wide=$(ROSETTA_ADVERTISE_AVX=1 "$OUT/probe")
case ",${wide#* }," in
    *,avx,*,fma,*,avx2,*bmi2,*) pass "H1 feature word: [$narrow] plain, [$wide] with ROSETTA_ADVERTISE_AVX=1" ;;
    *) echo "SKIP this Rosetta advertises no AVX2 ($wide)"; exit 0 ;;
esac

# --- H2: the oracle under the wider profile
make -s -C "$repo" B="$B" "$B/oracle-native" "$B/oracle-check" >/dev/null || exit 1
for m in simd dp; do
    timeout -s KILL 600 "$B/oracle-native" "$OUT/oracle-$m.bin" "$N" 0x$(date +%s)$RANDOM $m 2>/dev/null || { bad "H2 $m: oracle-native"; continue; }
    if ROSETTA_ADVERTISE_AVX=1 timeout -s KILL 600 "$B/oracle-check" "$OUT/oracle-$m.bin" --show 5 --bad "$OUT/bad-$m.txt" >"$OUT/oracle-$m.log"; then
        pass "H2 $m under ROSETTA_ADVERTISE_AVX=1: $(grep -o 'jit CPU profile.*' "$OUT/oracle-$m.log"); $(grep "^$m jit" "$OUT/oracle-$m.log" | grep -o 'native ran [0-9]*: match [0-9]*')"
    else bad "H2 $m: $(tail -3 "$OUT/oracle-$m.log")"; fi
done

# --- H3, H4: AOT caches
G=$OUT/hello.arm64e
clang -arch arm64e -O1 -o "$G" "$repo/tests/guest/hello.c" && codesign -s - -f "$G" 2>/dev/null || exit 1
WANT="hello from arm64e: argc=1 argv0=set pi=3.14159 ok"
for d in n w h; do rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"; chmod 700 "$OUT/$d"; done
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/n" --cpu=baseline "$G" >"$OUT/n.log" 2>&1
ROSETTA_ADVERTISE_AVX=1 timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/w" --cpu=baseline "$G" >"$OUT/w.log" 2>&1
NAME=$(cd "$OUT/n" && ls *.aot 2>/dev/null | head -1)
[ -n "$NAME" ] && cmp -s "$OUT/n/$NAME" "$OUT/w/$NAME" && pass "H3 --cpu=baseline: the same $(stat -f %z "$OUT/n/$NAME") bytes under both profiles" \
    || bad "H3 baseline caches differ or are missing ($NAME): $(tail -1 "$OUT/w.log")"
[ -n "$NAME" ] || exit 1
python3 -I "$here/patch.py" "$OUT/n/$NAME" "$OUT/h/$NAME" hdr.cpu_profile=0x83 || bad "H4 patch.py"
out=$(ROSETTA_ADVERTISE_AVX=1 HAO_VERBOSE=1 HAO_CACHE="$OUT/h" timeout -s KILL "$T" "$HAO" "$G" 2>"$OUT/h4w.err"); rc=$?
[ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "AOT cache .*$NAME loaded" "$OUT/h4w.err" \
    && pass "H4 a cache recording aes,pclmul,avx2 loads under the wider profile" || bad "H4 wide: rc=$rc $(grep -E 'AOT' "$OUT/h4w.err" | head -2)"
out=$(HAO_VERBOSE=1 HAO_CACHE="$OUT/h" timeout -s KILL "$T" "$HAO" "$G" 2>"$OUT/h4n.err"); rc=$?
[ $rc = 7 ] && [ "$out" = "$WANT" ] && grep -q "rejected: translated for CPU features this machine lacks: avx2$" "$OUT/h4n.err" \
    && pass "H4 ... and is rejected without it ($(grep -o 'lacks: .*' "$OUT/h4n.err")), the program runs translated" \
    || bad "H4 narrow: rc=$rc $(grep -E 'AOT' "$OUT/h4n.err" | head -2)"
exit $fail
