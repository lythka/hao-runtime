#!/bin/bash
# Run every fuzz target: replay the committed corpus (seeds and regression
# inputs), then fuzz each for FUZZ_TIME seconds from a scratch copy of it.
#   [B=build] [FUZZ_TIME=60] [OUT=dir] [SEED=n] tests/fuzz/run.sh
# Builds the targets with `make fuzz` first. Crashing inputs land in
# OUT/<target>/ (default $TMPDIR/hao-fuzz); exit status 1 if any target crashed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
B=${B:-build}
T=${FUZZ_TIME:-60}
OUT=${OUT:-${TMPDIR:-/tmp}/hao-fuzz}
SEED=${SEED:-$(date +%s)}
cd "$repo"
make -s B="$B" fuzz || exit 2
export ASAN_OPTIONS=${ASAN_OPTIONS:-symbolize=0:detect_leaks=0}
bad=0
run() {   # run NAME MAX_LEN
    local name=$1 max=$2 dir=$OUT/$1
    mkdir -p "$dir/corpus" "$dir/artifacts"
    cp "$here/corpus/${name#fuzz_}"/* "$dir/corpus/" 2>/dev/null
    local bin=$repo/$B/fuzz/$name
    if ! (cd "$dir" && TMPDIR=$dir timeout -s KILL 300 "$bin" -artifact_prefix="$dir/artifacts/" "$here/corpus/${name#fuzz_}"/* >"$dir/replay.log" 2>&1); then
        echo "FAIL $name: corpus replay crashed (see $dir/replay.log)"; bad=1; return
    fi
    (cd "$dir" && TMPDIR=$dir timeout -s KILL $((T + 120)) "$bin" -max_total_time="$T" -seed="$SEED" -max_len="$max" \
        -artifact_prefix="$dir/artifacts/" corpus >"$dir/fuzz.log" 2>&1)
    local rc=$? last
    last=$(grep '^fuzz: done' "$dir/fuzz.log" | tail -1)
    if [ $rc != 0 ] || [ -z "$last" ]; then
        echo "FAIL $name: rc $rc; $(ls "$dir/artifacts" | head -3 | tr '\n' ' ')(see $dir/fuzz.log)"; bad=1
    else echo "PASS $name: ${last#fuzz: done }"; fi
}
run fuzz_macho 65536
run fuzz_typeenc 1024
run fuzz_codesig 65536
run fuzz_ifunit 16384
mkdir -p "$OUT/fuzz_hao_fat"
if python3 -I "$here/fuzz_hao_fat.py" "$here"/corpus/fat/* >/dev/null 2>"$OUT/fuzz_hao_fat/replay.log" &&
   r=$(timeout -s KILL $((T + 60)) python3 -I "$here/fuzz_hao_fat.py" --time "$T" --seed "$SEED" \
        --artifacts "$OUT/fuzz_hao_fat" "$here/corpus/fat" "$here/corpus/macho" 2>&1); then
    echo "PASS fuzz_hao_fat: ${r##*: }"
else echo "FAIL fuzz_hao_fat: ${r:-replay crashed}"; bad=1; fi
exit $bad
