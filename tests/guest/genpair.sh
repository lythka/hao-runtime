#!/bin/sh
# Native generic results in guest storage (src/swiftcc.c hao_swift_genres_*, module-level functions): a native
# generic function returning (T?, U), a tuple Swift returns as one indirect result per element ahead of the
# formal arguments, makes T? = nil there with its own witnesses (tests/guest/genpair_lib.swift). With T = C? or
# CGImage? that is C?? / CGImage??'s outer nil, word 1 on arm64 and 2 on x86_64, next to a guest generic enum;
# guest code specialized for the types reads it inline. A function returning T? = nil in x8 too, also a one-byte
# one at an odd address. Guest nils native code forwards into its result (fullPair's U, same) or takes inout
# (replace) are lent as native-encoded copies and come back as the guest wrote them. The library is
# universal and loaded natively. The CGImage types' facts are the walk table's rows for 26B131 (its outer nil whole word
# 1 / 2), as a fixture (test switch HAO_SWIFT_WALK_FACTS, a TEST_SWITCHES build): the table applies only on its
# own cache. Every hao run (JIT, --interp, AOT) must print what native arm64 prints; BASE (a build without this
# conversion, optional) must differ.
#   OUT=dir HAO=/abs/build-ts/hao [BASE=/abs/old/hao] [T=60] tests/guest/genpair.sh
set -u
export HAO_NATIVE_APP_LIBS=1   # the library's x86_64 slice runs natively
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-genpair)}
HAO=${HAO:-$repo/build-ts/hao}
BASE=${BASE:-}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT"
d=$(cd "$OUT" && pwd)
fail=0
z=0000000000000000
cat >"$d/rows.facts" <<EOT
type So10CGImageRefa SAME POINTER 0
type So10CGImageRefaSg SAME POINTER 0
type So10CGImageRefaSgSg CONVERT POINTER 0
image So10CGImageRefaSgSg 0 $z $z $z $z
image So10CGImageRefaSgSg 1 0100000000000000 ffffffffffffffff 0200000000000000 ffffffffffffffff
EOT
export HAO_SWIFT_WALK_FACTS="$d/rows.facts"
mkdir -p "$d/aot" && chmod 700 "$d/aot"
for t in x86_64 arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -module-name GPLib -enable-library-evolution \
        -emit-module -emit-module-path "$d/mod-$t/GPLib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$d/libGPLib.dylib" -o "$d/lib-$t.dylib" "$here/genpair_lib.swift" \
        2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
done
lipo -create "$d/lib-x86_64.dylib" "$d/lib-arm64.dylib" "$d/lib-arm64e.dylib" -output "$d/libGPLib.dylib"
codesign -s - -f "$d/libGPLib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -I "$d/mod-$t" -L"$d" -lGPLib -o "$d/genpair.$t" \
        "$here/genpair.swift" 2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
    codesign -s - -f "$d/genpair.$t" 2>/dev/null
done
timeout -s KILL "$T" "$HAO" --aot --cache "$d/aot" "$d/genpair.arm64e" >/dev/null 2>&1 || echo "aot generation failed"
timeout -s KILL "$T" "$d/genpair.arm64" >"$d/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$d/native.out" | tr -d ' ')"
[ $nrc = 0 ] && [ "$(wc -l <"$d/native.out" | tr -d ' ')" = 11 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$d/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$d/genpair.arm64e" >"$d/$mode.out" 2>"$d/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' $mode "$rc" "$r"
    [ $r = SAME ] || { diff "$d/native.out" "$d/$mode.out" | head -8; tail -3 "$d/$mode.err" | cut -c1-300; } | sed 's/^/    /'
done
if [ -n "$BASE" ]; then
    timeout -s KILL "$T" env HAO_NO_AOT=1 "$BASE" "$d/genpair.arm64e" >"$d/base.out" 2>"$d/base.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/base.out"; then r=SAME; fail=1; else r=DIFF; fi
    printf '%-9s rc=%s %s (without the conversion)\n' base "$rc" "$r"
fi
[ $fail = 0 ] && echo "genpair: PASS" || echo "genpair: FAIL"
exit $fail
