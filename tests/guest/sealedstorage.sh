#!/bin/sh
# Array storage of Any holding nils whose encoding differs, its _ContiguousArrayStorage metadata in the guest
# image's sealed __DATA_CONST (tests/guest/sealedstorage.s; the toolchain emits no prespecialized metadata of
# stdlib classes), made by guest code and destroyed by native code, must equal native arm64 under JIT, --interp
# and AOT with no refusal; before the fix its destroy hook could not be set and the storage was refused.
# The program is arm64 natively and arm64e under hao.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/sealedstorage.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-sealedstorage)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    { xcrun clang -c -arch $a -mmacosx-version-min=27.0 -o "$OUT/meta.$a.o" "$here/sealedstorage.s" &&
      xcrun swiftc -O -parse-as-library -module-name sealedstorage -target $a-apple-macos27.0 -o "$OUT/sealedstorage.$a" \
          "$here/sealedstorage.swift" "$OUT/meta.$a.o"; } 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/sealedstorage.$a" 2>/dev/null
done
# the metadata must lie in a read-only-after-fixups segment
otool -l "$OUT/sealedstorage.arm64e" | grep -A9 'segname __DATA_CONST' | grep -qE '^ +flags 0x(00000)?10$' ||
    { echo "sealedstorage: __DATA_CONST is not SG_READ_ONLY"; exit 2; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/sealedstorage.arm64e" >"$OUT/aot.gen" 2>&1 ||
    { echo "sealedstorage: aot generation failed"; exit 2; }
timeout -s KILL "$T" "$OUT/sealedstorage.arm64" >"$OUT/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $nrc = 0 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" HAO_VERBOSE=1 "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/sealedstorage.arm64e" >"$OUT/$mode.out" 2>"$OUT/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$OUT/native.out" "$OUT/$mode.out" && ! grep -q 'Swift value boundary refused' "$OUT/$mode.err"
    then r=SAME; else r=DIFF; fail=1; fi
    if [ $mode = aot ] && ! grep -q "AOT cache .* loaded" "$OUT/$mode.err"; then r="$r (AOT cache not loaded)"; fail=1; fi
    printf '%-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || { diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -16; grep -a 'Swift value' "$OUT/$mode.err" | head -4 | cut -c1-300; }
done
[ $fail = 0 ] && echo "sealedstorage: PASS" || echo "sealedstorage: FAIL"
exit $fail
