#!/bin/sh
# The Swift value boundary policy (tests/guest/svpolicy.swift): guest generic code copies tuples through native
# tuple witnesses. "pass": tuples of a C?? beside a field of each pass class (owner-opaque, identical
# representation, inactive payload; named under HAO_BRIDGE_CHECK), and an Any holding a C?? walked through its
# dynamic type, equal native arm64 under JIT, --interp and AOT. Refused (the refusal line, then abort):
# "refuse" a C?? beside an InlineArray (no rule for its kind; with HAO_SWIFT_VALUE_PASSTHROUGH=1 it goes on, its
# sibling nil converted, equal native), "gate" an InlineArray beside a class reference only (no Optional to
# convert, still examined), "wide" a 257-element tuple (past the walk's bounds), "result" an AsyncStream's next()
# returning such a value to guest code, "boxed" an Any whose boxed value holds a C?? nil, destroyed in a tuple,
# "task" a child task with a C?? result (refused when created), "unreflected" a struct built without reflection
# metadata (tests/guest/svpolicy_lib.swift) beside a C?? (its fields are not known). "array" an array of C?? and
# "box" a captured C?? variable, "anyarray", "anycopy", "anyassign", "anybox": that storage and [Any] storage and a
# box of Any holding guest-written C?? nils, destroyed by the runtime (dropped, after a copy of the array value,
# assigned over): equal native arm64, converted at destruction. "frozen": a native frozen enum the guest built is
# examined (its case read, named under HAO_BRIDGE_CHECK), equal native arm64. The refusals run under the test
# switch HAO_SWIFT_VALUE_STRICT=1. By default a decline is reported and the value goes on as found.
# Native witnesses can misread an outer nil that required conversion.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svpolicy.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svpolicy)}
HAO=${HAO:-$repo/build-ts/hao}
# Uncertified values are refused here, as they are by default only once their rows land (a test build).
export HAO_SWIFT_VALUE_STRICT=1
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    mkdir -p "$OUT/mod-$a"
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -disable-reflection-metadata -module-name SvpolicyLib \
        -emit-module -emit-module-path "$OUT/mod-$a/SvpolicyLib.swiftmodule" -c -o "$OUT/svpolicy_lib.$a.o" \
        "$here/svpolicy_lib.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -I "$OUT/mod-$a" -o "$OUT/svpolicy.$a" \
        "$here/svpolicy.swift" "$OUT/svpolicy_lib.$a.o" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svpolicy.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svpolicy.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svpolicy: aot generation failed"
fail=0
for part in pass refuse gate wide result frozen boxed task array box unreflected anyarray anycopy anyassign anybox anymany; do
    timeout -s KILL "$T" "$OUT/svpolicy.arm64" $part >"$OUT/native.$part.out" 2>/dev/null; nrc=$?
    [ $nrc = 0 ] || { echo "$part native rc=$nrc"; fail=1; }
done
echo "native    rc=0 lines=$(cat "$OUT"/native.*.out | wc -l | tr -d ' ')"
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
    esac
    timeout -s KILL "$T" "$@" "$OUT/svpolicy.arm64e" pass >"$OUT/$mode.pass.out" 2>"$OUT/$mode.pass.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.pass.out" "$OUT/$mode.pass.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf 'pass   %-9s rc=%s %s\n' "$mode" "$rc" "$r"
    [ $r = SAME ] || diff "$OUT/native.pass.out" "$OUT/$mode.pass.out" | sed 's/^/    /' | head -16
done
# Each refused part: a nonzero exit, nothing printed past the boundary, the refusal line with its reason.
refused() {   # part mode reason command...
    part=$1 mode=$2 why=$3; shift 3
    timeout -s KILL "$T" "$@" "$OUT/svpolicy.arm64e" $part >"$OUT/$mode.$part.out" 2>"$OUT/$mode.$part.err"; rc=$?
    if [ $rc != 0 ] && [ ! -s "$OUT/$mode.$part.out" ] && grep -q "Swift value boundary refused: $why" "$OUT/$mode.$part.err"
    then r=REFUSED; else r=NOT-REFUSED; fail=1; fi
    printf '%-6s %-9s rc=%s %s\n' $part "$mode" "$rc" "$r"
    [ $r = REFUSED ] || grep -v hao-schema "$OUT/$mode.$part.err" | tail -3 | sed 's/^/    /'
}
refused refuse jit "unresolved active representation" env HAO_NO_AOT=1 "$HAO"
refused refuse interp "unresolved active representation" env HAO_NO_AOT=1 "$HAO" --interp
refused refuse aot "unresolved active representation" env HAO_CACHE="$OUT/aot" "$HAO"
refused gate jit "unresolved active representation" env HAO_NO_AOT=1 "$HAO"
refused wide jit "type graph past the walk's bounds" env HAO_NO_AOT=1 "$HAO"
refused result jit "native result has unresolved active representation" env HAO_NO_AOT=1 "$HAO"
refused boxed jit "guest-encoded nil in a shared box cannot be converted .*type=Any)" env HAO_NO_AOT=1 "$HAO"
refused task jit "child task result" env HAO_NO_AOT=1 "$HAO"
refused unreflected jit "value native code would misread .*type=(SvpolicyLib.Unreflected" env HAO_NO_AOT=1 "$HAO"
# The pass classes, named under HAO_BRIDGE_CHECK.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 "$HAO" "$OUT/svpolicy.arm64e" pass >"$OUT/check.out" 2>"$OUT/check.err"
for c in "owner-opaque .*NSObject" "identical-representation .*Int) -> Swift.Int" "inactive-payload .*Optional<Swift.Int>"; do
    grep -q "Swift value passed as found: $c" "$OUT/check.err" && r=NAMED || { r=NOT-NAMED; fail=1; }
    echo "class $c: $r"
done
# Storage holding guest-written nils (inside Any too), destroyed by the runtime: converted at that destruction.
for part in array box anyarray anycopy anyassign anybox anymany; do
    timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" "$OUT/svpolicy.arm64e" $part >"$OUT/jit.$part.out" 2>"$OUT/jit.$part.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.$part.out" "$OUT/jit.$part.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s jit rc=%s %s\n' $part "$rc" "$r"
    [ $r = SAME ] || grep -v hao-schema "$OUT/jit.$part.err" | tail -3 | sed 's/^/    /'
done
for part in array anyarray; do
    timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 "$HAO" "$OUT/svpolicy.arm64e" $part >/dev/null 2>"$OUT/$part.check"
    grep -q 'Swift storage destroyed by the runtime: [1-9][0-9]* nils converted' "$OUT/$part.check" && r=CONVERTED || { r=NOT-CONVERTED; fail=1; }
    echo "$part destruction $r"
done
# A native frozen enum is examined, not taken as native-made.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 "$HAO" "$OUT/svpolicy.arm64e" frozen >"$OUT/frozen.out" 2>"$OUT/frozen.err"; rc=$?
if [ $rc = 0 ] && cmp -s "$OUT/native.frozen.out" "$OUT/frozen.out" &&
   grep -q 'Swift value passed as found: .*Swift._PlaygroundQuickLook' "$OUT/frozen.err"; then r=EXAMINED; else r=NOT-EXAMINED; fail=1; fi
echo "frozen jit rc=$rc $r"
# The experimental pass-through: the value goes on, the certified sibling's nil still converted.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_VALUE_PASSTHROUGH=1 "$HAO" "$OUT/svpolicy.arm64e" refuse \
    >"$OUT/passthrough.out" 2>"$OUT/passthrough.err"; rc=$?
if [ $rc = 0 ] && cmp -s "$OUT/native.refuse.out" "$OUT/passthrough.out" &&
   grep -q 'partial conversion allowed (HAO_SWIFT_VALUE_PASSTHROUGH)' "$OUT/passthrough.err"; then r=SAME; else r=DIFF; fail=1; fi
echo "passthrough rc=$rc $r"
[ $r = SAME ] || { diff "$OUT/native.refuse.out" "$OUT/passthrough.out" | sed 's/^/    /' | head -8; tail -3 "$OUT/passthrough.err" | sed 's/^/    /'; }
# An InlineArray beside a live reference can be copied as found. The outer nil in "refuse"
# needs conversion for native ownership and is exercised above under strict and pass-through.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_VALUE_STRICT=0 "$HAO" "$OUT/svpolicy.arm64e" gate \
    >"$OUT/report.out" 2>"$OUT/report.err"; rc=$?
if [ $rc = 0 ] && cmp -s "$OUT/native.gate.out" "$OUT/report.out" && ! grep -q 'boundary refused' "$OUT/report.err" &&
   [ "$(grep -c 'not certified, passed as found (report-only): unresolved active representation' "$OUT/report.err")" = 1 ] &&
   grep -q '^hao-swift-audit .*"reason":"unresolved active representation' "$OUT/report.err"; then r=SAME; else r=DIFF; fail=1; fi
echo "report-only rc=$rc $r"
[ $r = SAME ] || tail -3 "$OUT/report.err" | sed 's/^/    /'
# Known-wrong values (a guest-encoded nil in a shared box, a child task's C?? result, an ObjC word that is no
# object) are reported and passed as found too outside strict mode; native code may then misread them (these
# constructed values can fault after the report; strict mode refuses them above).
reported() {   # part reason
    part=$1 why=$2
    timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_VALUE_STRICT=0 "$HAO" "$OUT/svpolicy.arm64e" $part \
        >"$OUT/default.$part.out" 2>"$OUT/default.$part.err"; rc=$?
    if ! grep -q 'boundary refused' "$OUT/default.$part.err" &&
       grep -q "not certified, passed as found (report-only[^)]*): $why" "$OUT/default.$part.err"
    then r=REPORTED; else r=NOT-REPORTED; fail=1; fi
    printf '%-6s default   rc=%s %s\n' $part "$rc" "$r"
    [ $r = REPORTED ] || grep -v hao-schema "$OUT/default.$part.err" | tail -3 | sed 's/^/    /'
}
reported boxed "guest-encoded nil in a shared box cannot be converted"
reported task "child task result has a nil native code would copy"
reported boxmix "guest-encoded nil in a shared box cannot be converted"
reported unrefbox "guest-encoded nil in a shared box cannot be converted"
reported unreflected "value native code would misread"
reported unrefopt "value native code would misread"
reported objcbad "value native code would misread"
[ $fail = 0 ] && echo "svpolicy: PASS" || echo "svpolicy: FAIL"
exit $fail
