#!/bin/sh
# The Swift call map (src/swiftcall.c): native Swift calls whose direct values have a nil whose encoding differs
# by architecture (tests/guest/swiftcallres_lib.swift). A getter returns (String?, String?)? in x0-x3 /
# rax rdx rcx r8, a setter and a function take one in x0-x3 / rdi rsi rdx rcx; the outer nil's word 1 is 1 on
# arm64 and 2 on x86_64; a throwing function returns one, or throws (its result registers then undefined: only
# the error crosses). The library is universal and loaded natively; its rows, keyed by its x86_64 LC_UUID,
# and the type's case images are a fixture (test switch HAO_SWIFT_WALK_FACTS, a TEST_SWITCHES build), in two
# builds of the library: "vtable" (the guest calls the implementations a vtable holds; rows by method
# descriptor, ...Tq) and "resilient" (library evolution: the guest calls dispatch thunks, rows ...Tj). With the
# rows every hao run (JIT, --interp, AOT) must print what native arm64 prints; without them (CTL, default
# build/hao) the run must differ. A REFUSE row is refused, and under HAO_SWIFT_VALUE_AUDIT=1 recorded once with
# the call's symbol and the library's UUID (the record's callee_symbol and callee_image). A fixture that permutes
# a function's argument and result registers must move each word where its row says.
#   OUT=dir HAO=/abs/build-ts/hao [CTL=/abs/build/hao] [T=60] tests/guest/swiftcallres.sh
set -u
export HAO_NATIVE_APP_LIBS=1   # the library's x86_64 slice runs natively
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftcallres)}
HAO=${HAO:-$repo/build-ts/hao}
CTL=${CTL:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
key=SSSg5title_AA8subtitletSg
z=0000000000000000
fail=0
for variant in vtable resilient; do
    d=$OUT/$variant
    mkdir -p "$d/aot" && chmod 700 "$d/aot"
    evo=; [ $variant = resilient ] && evo=-enable-library-evolution
    for t in x86_64 arm64 arm64e; do
        swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -module-name SCLib $evo \
            -emit-module -emit-module-path "$d/mod-$t/SCLib.swiftmodule" \
            -Xlinker -install_name -Xlinker "$d/libSCLib.dylib" -o "$d/lib-$t.dylib" "$here/swiftcallres_lib.swift" \
            2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
    done
    lipo -create "$d/lib-x86_64.dylib" "$d/lib-arm64.dylib" "$d/lib-arm64e.dylib" -output "$d/libSCLib.dylib"
    codesign -s - -f "$d/libSCLib.dylib" 2>/dev/null
    for t in arm64 arm64e; do
        swiftc -target $t-apple-macos14 -O -parse-as-library -I "$d/mod-$t" -L"$d" -lSCLib -o "$d/swiftcallres.$t" \
            "$here/swiftcallres.swift" 2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
        codesign -s - -f "$d/swiftcallres.$t" 2>/dev/null
    done
    U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$d/libSCLib.dylib")
    sym() { nm -arch x86_64 "$d/libSCLib.dylib" | grep -oE "_\\\$s5SCLib$1\$" | sed 's/^_//' | head -1; }
    if [ $variant = vtable ]; then get=$(sym '4CardC6labels[A-Za-z0-9_]*vgTq'); set=$(sym '4CardC6labels[A-Za-z0-9_]*vsTq'); kind=METHOD_DESCRIPTOR
    else get=$(sym '4CardC6labels[A-Za-z0-9_]*vgTj'); set=$(sym '4CardC6labels[A-Za-z0-9_]*vsTj'); kind=DISPATCH_THUNK; fi
    show=$(sym '4show[A-Za-z0-9_]*F'); pick=$(sym '4pick[A-Za-z0-9_]*KF'); pair=$(sym '8pairDiff[A-Za-z0-9_]*F')
    [ -n "$get" ] && [ -n "$set" ] && [ -n "$show" ] && [ -n "$pick" ] && [ -n "$pair" ] ||
        { echo "$variant: library symbols not found"; exit 2; }
    # rows (VERDICT the getter result's) and the type's case images: what the call map and the walk table hold
    facts() {
        cat <<EOF
type $key CONVERT
image $key 0 $z$z$z$z $z$z$z$z $z$z$z$z $z$z$z$z
image $key 1 ${z}0100000000000000$z$z ${z}ffffffffffffffff$z$z ${z}0200000000000000$z$z ${z}ffffffffffffffff$z$z
call $get $U $kind CONVERT
value r $key o $1 x0:rax x1:rdx x2:rcx x3:r8
value s 0 g SAME x20:r13
call $set $U $kind CONVERT
value p0 $key o CONVERT x0:rdi x1:rsi x2:rdx x3:rcx
value s 0 g SAME x20:r13
call $show $U FUNCTION CONVERT
value r SS o SAME x0:rax x1:rdx
value p0 $key g CONVERT x0:rdi x1:rsi x2:rdx x3:rcx
call $pick $U FUNCTION CONVERT
value r $key o CONVERT x0:rax x1:rdx x2:rcx x3:r8
value e 0 o SAME x21:r12
value p0 Si g SAME x0:rdi
EOF
    }
    facts CONVERT >"$d/rows.facts"
    facts REFUSE >"$d/refuse.facts"
    # A permutation no real lowering has, to see each word go where its row says: the two arguments swapped
    # (native pairDiff gets (2, 7)) and the two result words swapped (the guest gets (27, -5)).
    printf 'call %s %s FUNCTION SAME\nvalue p0 Si g SAME x0:rsi\nvalue p1 Si g SAME x1:rdi\nvalue r Si_Sit o SAME x0:rdx x1:rax\n' \
        "$pair" "$U" >"$d/perm.facts"
    HAO_SWIFT_WALK_FACTS="$d/rows.facts" timeout -s KILL "$T" "$HAO" --aot --cache "$d/aot" "$d/swiftcallres.arm64e" >/dev/null 2>&1 ||
        echo "$variant: aot generation failed"
    timeout -s KILL "$T" "$d/swiftcallres.arm64" >"$d/native.out" 2>/dev/null; nrc=$?
    echo "$variant native    rc=$nrc lines=$(wc -l <"$d/native.out" | tr -d ' ')"
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$d/aot" "$HAO" ;;
        esac
        HAO_SWIFT_WALK_FACTS="$d/rows.facts" timeout -s KILL "$T" "$@" "$d/swiftcallres.arm64e" >"$d/$mode.out" 2>"$d/$mode.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf '%s %-9s rc=%s %s\n' $variant $mode "$rc" "$r"
        [ $r = SAME ] || { diff "$d/native.out" "$d/$mode.out" | head -8; tail -3 "$d/$mode.err" | cut -c1-300; } | sed 's/^/    /'
    done
    for mode in jit interp; do
        case $mode in jit) set -- "$HAO" ;; interp) set -- "$HAO" --interp ;; esac
        HAO_SWIFT_WALK_FACTS="$d/perm.facts" timeout -s KILL "$T" env HAO_NO_AOT=1 "$@" "$d/swiftcallres.arm64e" perm \
            >"$d/perm.$mode.out" 2>"$d/perm.$mode.err"; rc=$?
        if [ $rc = 0 ] && [ "$(cat "$d/perm.$mode.out")" = "perm 27 -5" ]; then r=PERMUTED; else r=NOT-PERMUTED; fail=1; fi
        printf '%s %-9s rc=%s %s %s\n' $variant "perm-$mode" "$rc" "$r" "$(cat "$d/perm.$mode.out")"
    done
    # Without the rows the nils cross unconverted: the run must differ (the case the rows exist for).
    if [ -x "$CTL" ]; then
        timeout -s KILL "$T" env HAO_NO_AOT=1 "$CTL" "$d/swiftcallres.arm64e" >"$d/control.out" 2>"$d/control.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/control.out"; then r=SAME; fail=1; else r=DIFF; fi
        printf '%s %-9s rc=%s %s (without rows)\n' $variant control "$rc" "$r"
    fi
    # A REFUSE row: refused before the getter's first call; under the audit, one record names the call's
    # symbol and the library's UUID, and the run goes on as without a row (its nil unconverted: it may crash).
    HAO_SWIFT_WALK_FACTS="$d/refuse.facts" timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" "$d/swiftcallres.arm64e" \
        >"$d/refuse.out" 2>"$d/refuse.err"; rc=$?
    if [ $rc != 0 ] && [ ! -s "$d/refuse.out" ] &&
       grep -q "Swift value boundary refused: a value the call map refuses (call=$get " "$d/refuse.err"; then r=REFUSED
    else r=NOT-REFUSED; fail=1; fi
    printf '%s %-9s rc=%s %s\n' $variant refuse "$rc" "$r"
    HAO_SWIFT_WALK_FACTS="$d/refuse.facts" HAO_SWIFT_VALUE_AUDIT=1 timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" \
        "$d/swiftcallres.arm64e" >"$d/audit.out" 2>"$d/audit.err"; rc=$?
    UU=$(printf '%s' "$U" | tr a-f A-F)
    n=$(grep '^hao-swift-audit {"v":1,' "$d/audit.err" | grep -F '"reason":"a value the call map refuses"' |
        grep -F "\"callee_symbol\":\"$get\",\"callee_image\":\"$UU\"," | grep -c '"caller_image":"[0-9A-F]\{32\}"')
    if [ "$n" = 1 ] && ! grep -q 'Swift value boundary refused' "$d/audit.err"; then r=RECORDED; else r=NOT-RECORDED; fail=1; fi
    printf '%s %-9s rc=%s %s (%s record)\n' $variant audit "$rc" "$r" "$n"
    [ $r = RECORDED ] || grep -a '^hao-swift-audit' "$d/audit.err" | head -2 | cut -c1-500 | sed 's/^/    /'
done
[ $fail = 0 ] && echo "swiftcallres: PASS" || echo "swiftcallres: FAIL"
exit $fail
