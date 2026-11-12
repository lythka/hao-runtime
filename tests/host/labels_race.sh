#!/bin/sh
# A deterministic re-export reader versus a second name initialization.
# The mutation restores the faulty initialization guard in a private library.
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
OUT=${OUT:-$(mktemp -d -t hao-labels-race)}
N=${N:-1}
mkdir -p "$OUT"
make -s -j4 B="$OUT/build" TEST_SWITCHES=1 "$OUT/build/libhao.dylib"
clang -arch x86_64 -mmacosx-version-min=27.0 -O1 -Iinclude tests/host/labels_race.c \
    -L"$OUT/build" -lhao -Wl,-rpath,"$OUT/build" -o "$OUT/labels_race"
codesign -s - -f "$OUT/labels_race" 2>/dev/null
for mode in fixed reinit; do
    if [ "$mode" = reinit ]; then
        python3 - "$OUT/bridge-reinit.c" <<'PY'
from pathlib import Path
import sys
s = Path('src/bridge.c').read_text()
a = 'if (!lbl_names_ready) { labels_init(); __atomic_store_n(&lbl_names_ready, true, __ATOMIC_RELEASE); }'
assert s.count(a) == 1
Path(sys.argv[1]).write_text(s.replace(a, 'if (!lbl_ready) labels_init();'))
PY
        clang -arch x86_64 -mmacosx-version-min=27.0 -march=x86-64-v2 -std=gnu11 -O2 \
            -Iinclude -Isrc -DHAO_TEST_SWITCHES -c "$OUT/bridge-reinit.c" -o "$OUT/build/bridge.o"
        make -s B="$OUT/build" TEST_SWITCHES=1 "$OUT/build/libhao.dylib"
    fi
    python3 - "$OUT" "$mode" "$N" <<'PY'
import pathlib,subprocess,sys,json
out=pathlib.Path(sys.argv[1]);mode=sys.argv[2];n=int(sys.argv[3]);results=[]
for k in range(n):
    p=subprocess.run(['timeout','-s','KILL','20',str(out/'labels_race')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    results.append(p.returncode)
    (out/f'{mode}-{k:03}.log').write_bytes(p.stdout)
(out/f'{mode}-results.json').write_text(json.dumps(results))
crashes=sum(x in (-11,-10,139,138) for x in results)
print(f'{mode}: {n} runs, {crashes} crashes, {sum(x == 0 for x in results)} passes')
if mode=='fixed': assert all(x==0 for x in results),results
else: assert crashes==n,results
PY
done
