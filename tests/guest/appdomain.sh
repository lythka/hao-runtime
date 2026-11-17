#!/bin/sh
# App libraries and the guest representation domain: OS-path classification by the resolved path, HAO_NATIVE_APP_LIBS ignored for
# guests that require library validation, native callers resolving natively first, no second instance of a native-loaded library.
#   OUT=dir HAO=/abs/build/hao tests/guest/appdomain.sh
set -eu
unset HAO_NATIVE_APP_LIBS
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
HAO=${HAO:-$repo/build/hao}
OUT=${OUT:-$(mktemp -d -t hao-appdomain)}
mkdir -p "$OUT/arm" "$OUT/override"
OUT=$(cd "$OUT" && pwd -P)
clang -arch arm64e -O1 -o "$OUT/main" "$here/appdomain.c"
clang -arch x86_64 -O1 -dynamiclib -DHELPER -o "$OUT/helper.dylib" "$here/appdomain.c"
clang -arch x86_64 -arch arm64e -O1 -dynamiclib -DPROBE -o "$OUT/libprobe.dylib" "$here/appdomain.c"
clang -arch arm64e -O1 -dynamiclib -DPROBE -o "$OUT/arm/libprobe.dylib" "$here/appdomain.c"
cp "$OUT/libprobe.dylib" "$OUT/override/libprobe.dylib"
for f in "$OUT/main" "$OUT/helper.dylib" "$OUT/libprobe.dylib" "$OUT/arm/libprobe.dylib" "$OUT/override/libprobe.dylib"; do
    codesign -s - -f "$f"
done
alias=/System/Volumes/Data$OUT/libprobe.dylib
[ -f "$alias" ] || { echo "OUT must be on the Data volume" >&2; exit 1; }
for mode in jit interp; do
    set -- "$HAO" -v
    [ "$mode" != interp ] || set -- "$@" --interp
    env HAO_NO_AOT=1 "$@" "$OUT/main" alias "$OUT/helper.dylib" "$alias"
    env HAO_NO_AOT=1 "$@" "$OUT/main" alias "$OUT/helper.dylib" "/System/..$OUT/libprobe.dylib"
    env HAO_NO_AOT=1 "$@" "$OUT/main" native "$OUT/helper.dylib" "$OUT/libprobe.dylib"
    # ARM-only file: dyld's native search override must win for a native caller.
    env HAO_NO_AOT=1 DYLD_LIBRARY_PATH="$OUT/override" "$@" "$OUT/main" native "$OUT/helper.dylib" "$OUT/arm/libprobe.dylib"
    env HAO_NO_AOT=1 "$@" "$OUT/main" duplicate "$OUT/helper.dylib" "$OUT/libprobe.dylib" 2>"$OUT/$mode.err"
    [ "$(grep -c 'stays native: its native slice is loaded already' "$OUT/$mode.err")" = 1 ]
done
for policy in runtime library; do
    cp "$OUT/main" "$OUT/main.$policy"
    codesign -s - -f -o "$policy" "$OUT/main.$policy"
    # Startup may be refused by LV on the coordinator's platform; the option
    # diagnostic must already have happened, once, before any dependency load.
    env HAO_NATIVE_APP_LIBS=1 "$HAO" "$OUT/main.$policy" >"$OUT/$policy.out" 2>"$OUT/$policy.err" || :
    [ "$(grep -c 'HAO_NATIVE_APP_LIBS ignored: the program requires library validation' "$OUT/$policy.err")" = 1 ]
done
echo 'appdomain: PASS'
