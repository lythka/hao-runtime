#!/bin/sh
# Native Swift functions whose Apple arm64 stack arguments are packed at natural size, called by guest
# code with no interface unit for them (HAO_NO_IFUNITS: no record may supply the layout): the packing
# comes from the guest call site's stores, checked against the slots the x86_64 callee reads.
#  - tests/guest/sitepack.swift, helper sitepackhelper.swift (library evolution): friend (FindMyLocate
#    Friend.init's shape), spill (a `str q0` spill above the arguments in the call sequence), encode
#    (AppSubscriptions UserSubscriptionContextData.init's shape: four constant Int32 zeros in one
#    `stp xzr, xzr, [sp, #-32]!`, then an indirect Offer?, the values encoded by the native
#    JSONEncoder inside a guest Codable value), and pair0, pair16, quad, whose constant Bools LLVM
#    merges into one strh or str w, and method (
#    an open class's method through its dispatch thunk, which reads no stack itself; the
#    implementation its method descriptor names does), with nil String? and [String]? arguments the compiler
#    stores from a zeroed SIMD register (`stp q0, q0`, `stur q0`) among the merged Bool? stores.
#  - tests/guest/stackpack.swift, helper stackpackhelper.swift (no library evolution): opts and mgr
#    (three constant Bools, two of them merged), mixed (Int16/Bool/Int32/UInt8/Int64/Bool), box (a
#    class method; its merged Bools end at a multiple of 8).
# Each case runs in its own process natively on arm64 and under hao on arm64e (JIT, --interp, AOT),
# with HAO_BRIDGE_CHECK=1. A case whose call sites hao gave a layout must equal native arm64; one it
# refused got the word copy as before (its output is not compared). The expected verdict per case is
# below: a merged store must never be taken for one argument; it is split by the callee's read
# widths when they fill it exactly.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/sitepack.sh
set -u
# This fixture exercises the guest/native bridge through its universal helper.
export HAO_NATIVE_APP_LIBS=1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-sitepack)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)

# sitepack: library evolution
L=$OUT/libSitePackHelper.dylib
for a in x86_64 arm64 arm64e; do
    xcrun swiftc -target $a-apple-macos27.0 -O -enable-library-evolution -parse-as-library -module-name SitePackHelper \
        -emit-module -emit-module-path "$OUT/mod-$a/SitePackHelper.swiftmodule" -emit-library -o "$OUT/h-$a.dylib" \
        -Xlinker -install_name -Xlinker "$L" "$here/sitepackhelper.swift" 2>"$OUT/build.h-$a.err" \
        || { cat "$OUT/build.h-$a.err"; exit 2; }
done
lipo -create "$OUT/h-x86_64.dylib" "$OUT/h-arm64.dylib" "$OUT/h-arm64e.dylib" -output "$L" || exit 2
codesign -s - -f "$L" 2>/dev/null
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -I "$OUT/mod-$a" -L "$OUT" -lSitePackHelper \
        -o "$OUT/sitepack.$a" "$here/sitepack.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/sitepack.$a" 2>/dev/null
done

# stackpack: a private module (no library evolution, no interface)
L=$OUT/libstackpackhelper.dylib
mkdir -p "$OUT/sph" "$OUT/spmod/StackPackHelper.swiftmodule"
for a in x86_64 arm64 arm64e; do
    xcrun swiftc -target $a-apple-macos27.0 -O -parse-as-library -module-name StackPackHelper -emit-module \
        -emit-module-path "$OUT/spmod/StackPackHelper.swiftmodule/$a-apple-macos.swiftmodule" \
        -emit-library -o "$OUT/sph/$a.dylib" -Xlinker -install_name -Xlinker "$L" "$here/stackpackhelper.swift" \
        2>"$OUT/build.sph-$a.err" || { cat "$OUT/build.sph-$a.err"; exit 2; }
done
lipo -create "$OUT/sph/x86_64.dylib" "$OUT/sph/arm64.dylib" "$OUT/sph/arm64e.dylib" -output "$L" || exit 2
codesign -s - -f "$L" 2>/dev/null
for a in arm64e arm64; do
    xcrun swiftc -target $a-apple-macos27.0 -O -I "$OUT/spmod" -L "$OUT" -lstackpackhelper \
        -o "$OUT/stackpack.$a" "$here/stackpack.swift" 2>"$OUT/build.sp-$a.err" || { cat "$OUT/build.sp-$a.err"; exit 2; }
    codesign -s - -f "$OUT/stackpack.$a" 2>/dev/null
done

fail=0
for p in sitepack stackpack; do
    rm -rf "$OUT/aot-$p"; mkdir -m 700 "$OUT/aot-$p"
    HAO_NO_IFUNITS=1 timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot-$p" "$OUT/$p.arm64e" >"$OUT/aot-$p.gen" 2>&1 \
        || { echo "$p: aot generation failed"; fail=1; }
done

# PROGRAM CASE VERDICT: take (a layout at every packed site, equal to native), refuse (word copy at
# every such site, none taken), copy (no site looks packed: the word copy, unchanged)
printf '%-10s %-7s %-7s %s\n' program case expect "jit interp aot"
while read -r p c want; do
    timeout -s KILL "$T" "$OUT/$p.arm64" "$c" </dev/null >"$OUT/$p-$c.native" 2>/dev/null; nrc=$?
    [ $nrc = 0 ] || { echo "$p $c: native rc=$nrc"; fail=1; }
    line=$(printf '%-10s %-7s %-7s' "$p" "$c" "$want")
    for mode in jit interp aot; do
        # The programs build arrays of tuples holding an Optional of the helper's own struct, which no
        # frozen-struct table covers: hao refuses their storage by default (swiftvalue.c storage_element).
        # Packed stack arguments are the subject here, so the experimental pass-through lets them go on.
        case $mode in
        jit) set -- env HAO_NO_AOT=1 HAO_SWIFT_VALUE_PASSTHROUGH=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 HAO_SWIFT_VALUE_PASSTHROUGH=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot-$p" HAO_SWIFT_VALUE_PASSTHROUGH=1 "$HAO" ;;
        esac
        o=$OUT/$p-$c.$mode
        (HAO_NO_IFUNITS=1 HAO_BRIDGE_CHECK=1 timeout -s KILL "$T" "$@" "$OUT/$p.arm64e" "$c" </dev/null >"$o.out" 2>"$o.err"
         echo $? >"$o.rc") 2>/dev/null
        rc=$(cat "$o.rc")
        took=$(grep -c 'packed stack arguments from the call site' "$o.err")
        refused=$(grep -c 'packed stack arguments at .* refused' "$o.err")
        if [ $rc = $nrc ] && cmp -s "$OUT/$p-$c.native" "$o.out"; then r=SAME; else r="DIFF(rc=$rc)"; fi
        ok=1
        case $want in
        take) [ "$took" -gt 0 ] && [ "$refused" = 0 ] && [ $r = SAME ] || ok=0 ;;
        refuse) [ "$took" = 0 ] && [ "$refused" -gt 0 ] || ok=0 ;;
        copy) [ "$took" = 0 ] && [ "$refused" = 0 ] || ok=0 ;;
        esac
        [ "$took" -gt 0 ] && [ $r != SAME ] && ok=0             # a layout taken must be right
        line="$line $mode:$r/took=$took/refused=$refused"
        if [ $ok = 0 ]; then
            fail=1
            echo "$p $c $mode: expected $want"
            grep 'packed stack arguments' "$o.err" | sed 's/^/    /' | head -8
            [ $r = SAME ] || diff "$OUT/$p-$c.native" "$o.out" | sed 's/^/    /' | head -8
        fi
    done
    echo "$line"
done <<'EOF'
sitepack friend take
sitepack spill take
sitepack encode take
sitepack pair0 take
sitepack pair16 take
sitepack quad take
sitepack method take
stackpack mixed take
stackpack opts take
stackpack mgr take
stackpack box take
EOF
[ $fail = 0 ] && echo "sitepack: PASS" || echo "sitepack: FAIL"
exit $fail
