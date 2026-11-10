#!/bin/sh
# Runtime bookkeeping (runtime.c):
#   traps-unresolved  one trap per unresolved symbol across images; a call
#                     through it names the symbol (tests/host/traps.c)
#   traps-many        70000 traps: the table fills without aborting
#   stacks-jit/interp tests/guest/stacks.c: LC_MAIN stacksize and
#                     pthread_attr_setstacksize size the guest stacks (40 MB
#                     recursions), output as native arm64
# Prints PASS/FAIL per check; exit 0 if all pass, 1 otherwise.
#   OUT=dir LIB=build [HAO=build/hao] [INC=include] tests/host/runtime.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-runtime)}
LIB=$(cd "${LIB:-$repo/build}" && pwd)
HAO=${HAO:-$LIB/hao}
INC=${INC:-$repo/include}
mkdir -p "$OUT"
fail=0
check() {  # name ok(0/1) detail
    if [ "$2" = 1 ]; then r=PASS; else r=FAIL; fail=1; fi
    printf '%-17s %s  %s\n' "$1" "$r" "$3"
}
T=$OUT/test_traps
clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1 -I"$INC" -o "$T" "$here/traps.c" \
    -L"$LIB" -lhao -Wl,-rpath,"$LIB" || { echo "build failed"; exit 1; }
codesign -s - -f "$T" 2>/dev/null
for m in unresolved many; do
    timeout -s KILL 120 "$T" $m "$OUT" >"$OUT/traps-$m.out" 2>"$OUT/traps-$m.err"
    rc=$?
    line=$(tail -1 "$OUT/traps-$m.out")
    ok=0; [ $rc = 0 ] && echo "$line" | grep -q ' -> PASS$' && ok=1
    check traps-$m $ok "rc=$rc ${line:-(no summary)}"
done

for a in arm64e arm64; do
    clang -arch $a -O1 -Wl,-stack_size,0x4000000 -o "$OUT/stacks.$a" "$repo/tests/guest/stacks.c" &&
        codesign -s - -f "$OUT/stacks.$a" 2>/dev/null || { echo "guest build failed"; exit 1; }
done
timeout -s KILL 60 "$OUT/stacks.arm64" >"$OUT/stacks.native.out" 2>&1
for mode in jit interp; do
    fl=; [ $mode = interp ] && fl=--interp
    timeout -s KILL 300 "$HAO" $fl "$OUT/stacks.arm64e" >"$OUT/stacks.$mode.out" 2>"$OUT/stacks.$mode.err"
    rc=$?
    ok=0; [ $rc = 0 ] && cmp -s "$OUT/stacks.native.out" "$OUT/stacks.$mode.out" && ok=1
    check stacks-$mode $ok "rc=$rc $(tr '\n' ' ' <"$OUT/stacks.$mode.out")(native: $(tr '\n' ' ' <"$OUT/stacks.native.out"| sed 's/ $//'))"
done
exit $fail
