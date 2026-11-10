#!/bin/sh
# Production aggregate table, no interface-unit override; native arm64 oracle.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-sret-forward)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for arch in x86_64 arm64 arm64e; do
    clang -arch "$arch" -O2 -DHELPER -c "$here/sret-forward.c" -o "$OUT/helper.$arch.o"
    clang -arch "$arch" -O2 -DLIBRARY -c "$here/sret-forward.c" -o "$OUT/library.$arch.o"
    clang -arch "$arch" -dynamiclib -install_name "$OUT/libsret-forward.dylib" \
        "$OUT/helper.$arch.o" "$OUT/library.$arch.o" -o "$OUT/lib.$arch.dylib"
done
lipo -create "$OUT/lib.x86_64.dylib" "$OUT/lib.arm64.dylib" "$OUT/lib.arm64e.dylib" -output "$OUT/libsret-forward.dylib"
codesign -s - -f "$OUT/libsret-forward.dylib" 2>/dev/null
for arch in arm64 arm64e; do
    clang -arch "$arch" -O2 "$here/sret-forward.c" -L"$OUT" -lsret-forward -o "$OUT/guest.$arch"
    codesign -s - -f "$OUT/guest.$arch" 2>/dev/null
done
for case in 0 1 2 3 4 5 6 7 8 9 10 11; do
    timeout -s KILL "$T" "$OUT/guest.arm64" "$case" > "$OUT/native.$case.out"
    test "$(wc -l < "$OUT/native.$case.out" | tr -d ' ')" = 3
done
export HAO_NO_IFUNITS=1 HAO_CACHE="$OUT/aot"
mkdir -p "$HAO_CACHE"
timeout -s KILL "$T" "$HAO" --aot --cache "$HAO_CACHE" "$OUT/guest.arm64e" > "$OUT/aotgen.log" 2>&1
fail=0
for mode in jit interp aot; do
    flag=; [ "$mode" != interp ] || flag=--interp
    # any HAO_NO_AOT value turns AOT off (src/opts.c: a switch is on when present)
    no_aot=HAO_NO_AOT=1; [ "$mode" != aot ] || no_aot=
    for case in 0 1 2 3 4 5 6 7 8 9 10 11; do
        if env $no_aot HAO_VERBOSE=1 timeout -s KILL "$T" "$HAO" $flag "$OUT/guest.arm64e" "$case" \
            > "$OUT/$mode.$case.out" 2> "$OUT/$mode.$case.err" &&
            cmp -s "$OUT/native.$case.out" "$OUT/$mode.$case.out"; then
            echo "sret-forward $mode case=$case SAME, result and input canaries intact"
        else
            echo "sret-forward $mode case=$case FAIL (see $OUT/$mode.$case.err)"
            fail=1
        fi
        if [ "$mode" = aot ] && ! grep -q 'AOT cache .* loaded' "$OUT/$mode.$case.err"; then
            echo "sret-forward aot case=$case FAIL (no AOT cache loaded)"; fail=1
        fi
    done
done
exit "$fail"
