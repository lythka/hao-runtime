#!/bin/sh
# Guest closures passed to native generic Swift functions (tests/guest/swiftcallee.swift, helper
# swiftcalleehelper.swift) return their T through the caller's indirect result buffer. The native
# helper's symbol table is emptied (its export trie stays), so dladdr names none of its functions, as
# for many shared-cache functions; the guest is stripped of local symbols, so its closures carry no
# Swift name either. The callee is known as Swift only by the import the guest bound. Built for arm64
# (reference, native) and arm64e (under hao: JIT, --interp, AOT); each must print what native prints.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/swiftcallee.sh
set -u
export HAO_NATIVE_APP_LIBS=1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftcallee)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
L=$OUT/libSwiftCalleeHelper.dylib
for a in x86_64 arm64 arm64e; do
    xcrun swiftc -target $a-apple-macos27.0 -O -parse-as-library -module-name SwiftCalleeHelper \
        -emit-module -emit-module-path "$OUT/mod-$a/SwiftCalleeHelper.swiftmodule" -emit-library -o "$OUT/h-$a.dylib" \
        -Xlinker -install_name -Xlinker "$L" "$here/swiftcalleehelper.swift" 2>"$OUT/build.h-$a.err" \
        || { cat "$OUT/build.h-$a.err"; exit 2; }
done
# empty the x86_64 symbol table: LC_SYMTAB nsyms and LC_DYSYMTAB's ranges (no undefined or indirect symbols)
python3 - "$OUT/h-x86_64.dylib" <<'PY' || exit 2
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
assert struct.unpack_from('<I', d, 0)[0] == 0xfeedfacf
ncmds, o = struct.unpack_from('<I', d, 16)[0], 32
for _ in range(ncmds):
    cmd, cs = struct.unpack_from('<2I', d, o)
    if cmd == 0x2: struct.pack_into('<I', d, o + 12, 0)
    if cmd == 0xb:
        assert struct.unpack_from('<I', d, o + 28)[0] == 0 and struct.unpack_from('<I', d, o + 60)[0] == 0
        struct.pack_into('<6I', d, o + 8, 0, 0, 0, 0, 0, 0)
    o += cs
open(sys.argv[1], 'wb').write(d)
PY
lipo -create "$OUT/h-x86_64.dylib" "$OUT/h-arm64.dylib" "$OUT/h-arm64e.dylib" -output "$L" || exit 2
codesign -s - -f "$L" 2>/dev/null
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -I "$OUT/mod-$a" -L "$OUT" -lSwiftCalleeHelper \
        -o "$OUT/swiftcallee.$a" "$here/swiftcallee.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    strip -x "$OUT/swiftcallee.$a"
    codesign -s - -f "$OUT/swiftcallee.$a" 2>/dev/null
done
if nm "$OUT/h-x86_64.dylib" 2>/dev/null | grep -q .; then echo "swiftcallee: the helper keeps symbols"; exit 1; fi
fail=0
timeout -s KILL "$T" "$OUT/swiftcallee.arm64" >"$OUT/native.out" 2>"$OUT/native.err" || { echo "native failed"; fail=1; }
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL 120 "$HAO" --aot --cache "$OUT/aot" "$OUT/swiftcallee.arm64e" >"$OUT/aot.gen" 2>&1 || { echo "aot generation failed"; fail=1; }
for m in jit interp aot; do
    case $m in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/swiftcallee.arm64e" >"$OUT/$m.out" 2>"$OUT/$m.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$m.out"; then echo "$m: SAME"; else
        echo "$m: DIFF rc=$rc"; fail=1
        diff "$OUT/native.out" "$OUT/$m.out" | head -6 | sed 's/^/    /'; tail -3 "$OUT/$m.err" | sed 's/^/    /'
    fi
done
[ $fail = 0 ] && echo "swiftcallee: PASS" || echo "swiftcallee: FAIL"
exit $fail
