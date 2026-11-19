#!/bin/sh
# Persistent hint redirection must not disable JIT or async records.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
LIB=$(cd "${LIB:-$repo/build}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-jit-arena)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
clang -arch x86_64 -mmacosx-version-min=27.0 -dynamiclib "$here/jit-arena-map.c" \
    -install_name @rpath/libarena-map.dylib -o "$OUT/libarena-map.dylib"
codesign -s - -f "$OUT/libarena-map.dylib"
clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1 -Wall -Wextra -I"$repo/include" \
    "$here/jit-arena.c" -DWX_LIB "$here/../guest/wx.c" -L"$OUT" -larena-map -Wl,-rpath,"$OUT" -L"$LIB" -lhao -Wl,-rpath,"$LIB" -Wl,-rpath,@executable_path -o "$OUT/jit-arena"
codesign -s - -f "$OUT/jit-arena"
for mb in 1 256 1024; do
    timeout -s KILL 120 env HAO_JIT_CACHE_MB=$mb "$OUT/jit-arena"
done
for stage in 1 2; do
    timeout -s KILL 120 "$OUT/jit-arena" "$stage" >"$OUT/failure-$stage.out" 2>"$OUT/failure-$stage.err"
    cat "$OUT/failure-$stage.out"
done
grep -q 'JIT arena data protection failed (mach_vm_protect:' "$OUT/failure-1.err"
grep -q 'no W^X mapping for translated code' "$OUT/failure-2.err"
# Exercise both users of the async record pool under the redirected-data policy,
# comparing real Swift values and callbacks against the native ARM executions.
OUT="$OUT/async" HAO="$LIB/hao" \
    HAO_ENV="DYLD_INSERT_LIBRARIES=$OUT/libarena-map.dylib HAO_FLAGS=s" \
    "$here/../guest/run-async.sh" -k 2 14
for test in async-2-taskgroup async-14-native-callers; do
    grep -Eq ' task_entry=[1-9][0-9]* task_entry_unmade=0 ' "$OUT/async/$test.hao.err"
    grep -Eq ' async_arg=[1-9][0-9]* ' "$OUT/async/$test.hao.err"
done
