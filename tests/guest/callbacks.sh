#!/bin/sh
# Guest callbacks that reach native code outside x0-x7: cbstructs.m (C
# callback structures, static and built at run time), fnslots.mm (static
# tables, pointer identity, C++ vtables, blocks in structures and stack
# arguments, dynamic struct-returning IMPs; native helper fnhelper.mm) and
# swiftgen.swift (Swift generics: value and protocol witnesses, destroy
# functions, closure destructors).
# Each is built for arm64e (guest) and arm64 (reference); arm64 runs
# natively, arm64e under hao with HAO_NX_STATS=1:
#   jit      default
#   interp   --interp
#   nofix    HAO_NO_FIXUP_THUNKS=1 (load-time slot rewrite off)
#   off      HAO_NO_FIXUP_THUNKS=1 HAO_NO_STRUCT_THUNKS=1 HAO_NO_BLOCK_THUNKS=1
#   base     $HAO_BASE, if set
# Prints SAME/DIFF against native arm64, the NX fault line and the
# fnslots summary.
#   OUT=dir HAO=build/hao [HAO_BASE=/path/to/old/hao] [T=60] [PROGS="cbstructs fnslots swiftgen"] tests/guest/callbacks.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-callbacks}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
PROGS=${PROGS:-cbstructs fnslots swiftgen}
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" -framework Foundation \
    -install_name "$OUT/libfnhelper.dylib" -o "$OUT/libfnhelper.dylib" "$here/fnhelper.mm"
codesign -s - -f "$OUT/libfnhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -Wall -Wno-deprecated-declarations -o "$OUT/cbstructs.$a" "$here/cbstructs.m" \
        -framework Foundation -framework CoreGraphics -framework CoreText -framework CoreServices \
        -framework SystemConfiguration -framework CFNetwork -framework AudioToolbox -lz -lbz2 -llzma -lsqlite3
    clang++ -arch $a -O1 -std=c++17 -I"$here" -framework Foundation -L"$OUT" -lfnhelper \
        -o "$OUT/fnslots.$a" "$here/fnslots.mm"
    xcrun swiftc -target $a-apple-macos14.0 -O -o "$OUT/swiftgen.$a" "$here/swiftgen.swift"
    codesign -s - -f "$OUT/cbstructs.$a" "$OUT/fnslots.$a" "$OUT/swiftgen.$a" 2>/dev/null
done
set +e
run() {  # args name label cmd...
    rn=$1; rl=$2; shift 2
    (cd /tmp && timeout -s KILL "$T" "$@") >"$OUT/$rn.$rl.out" 2>"$OUT/$rn.$rl.err"
    rc=$?
    if cmp -s "$OUT/$rn.native.out" "$OUT/$rn.$rl.out"; then rr=SAME; else rr=DIFF; fi
    nx=$(grep 'nx faults' "$OUT/$rn.$rl.err" | head -1 | sed 's/^hao: nx faults: //')
    fs=$(grep 'hao-fnslots:' "$OUT/$rn.$rl.err" | head -1 | sed 's/^hao-fnslots: //')
    printf '%-10s %-6s rc=%-3s %-4s %s | %s\n' "$rn" "$rl" "$rc" "$rr" "$nx" "$fs"
}
for p in $PROGS; do
    (cd /tmp && timeout -s KILL "$T" "$OUT/$p.arm64") >"$OUT/$p.native.out" 2>"$OUT/$p.native.err"
    echo "$p native rc=$?"
    run $p jit    env HAO_NX_STATS=1 "$HAO" --stats "$OUT/$p.arm64e"
    run $p interp env HAO_NX_STATS=1 "$HAO" --stats --interp "$OUT/$p.arm64e"
    run $p nofix  env HAO_NX_STATS=1 HAO_NO_FIXUP_THUNKS=1 "$HAO" --stats "$OUT/$p.arm64e"
    run $p off    env HAO_NX_STATS=1 HAO_NO_FIXUP_THUNKS=1 HAO_NO_STRUCT_THUNKS=1 HAO_NO_BLOCK_THUNKS=1 "$HAO" --stats "$OUT/$p.arm64e"
    if [ -n "${HAO_BASE:-}" ]; then
        run $p base env HAO_NX_STATS=1 "$HAO_BASE" --stats "$OUT/$p.arm64e"
    fi
done
