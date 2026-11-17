#!/bin/sh
# The Swift value boundary policy (tests/guest/svpolicy.swift): guest generic code copies tuples holding a
# C?? through native tuple witnesses. "pass": a tuple whose other field is in a pass class (owner-opaque,
# identical representation, inactive payload) is converted and must equal native arm64, each class named
# under HAO_BRIDGE_CHECK. "refuse": a tuple with live weak ObjC storage is refused (the refusal line, then
# abort) by default, and with HAO_SWIFT_VALUE_PASSTHROUGH=1 goes on with its nil still converted, equal to
# native arm64. The program is arm64 natively and arm64e under hao (JIT, --interp, AOT).
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/svpolicy.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svpolicy)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -target $a-apple-macos27.0 -o "$OUT/svpolicy.$a" "$here/svpolicy.swift" \
        2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svpolicy.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svpolicy.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svpolicy: aot generation failed"
fail=0
for part in pass refuse; do
    timeout -s KILL "$T" "$OUT/svpolicy.arm64" $part >"$OUT/native.$part.out" 2>/dev/null; nrc=$?
    echo "$part native    rc=$nrc lines=$(wc -l <"$OUT/native.$part.out" | tr -d ' ')"
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/svpolicy.arm64e" $part >"$OUT/$mode.$part.out" 2>"$OUT/$mode.$part.err"; rc=$?
        if [ $part = pass ]; then
            if [ $rc = $nrc ] && cmp -s "$OUT/native.$part.out" "$OUT/$mode.$part.out"; then r=SAME; else r=DIFF; fail=1; fi
            printf '%s %-9s rc=%s %s\n' $part "$mode" "$rc" "$r"
            [ $r = SAME ] || diff "$OUT/native.$part.out" "$OUT/$mode.$part.out" | sed 's/^/    /' | head -16
        else
            # refused before the first line; no output past the boundary
            if [ $rc != 0 ] && [ ! -s "$OUT/$mode.$part.out" ] &&
               grep -q 'Swift value boundary refused: .*type=svpolicy.W' "$OUT/$mode.$part.err"; then r=REFUSED
            else r=NOT-REFUSED; fail=1; fi
            printf '%s %-9s rc=%s %s\n' $part "$mode" "$rc" "$r"
            [ $r = REFUSED ] || tail -5 "$OUT/$mode.$part.err" | sed 's/^/    /'
        fi
    done
done
# The pass classes, named under HAO_BRIDGE_CHECK.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_BRIDGE_CHECK=1 "$HAO" "$OUT/svpolicy.arm64e" pass >"$OUT/check.out" 2>"$OUT/check.err"
for c in "owner-opaque .*NSObject" "identical-representation .*Int) -> Swift.Int" "inactive-payload .*Optional<Swift.Int>"; do
    grep -q "Swift value passed as found: $c" "$OUT/check.err" && r=NAMED || { r=NOT-NAMED; fail=1; }
    echo "class $c: $r"
done
# The experimental pass-through: the value goes on, the certified sibling's nil still converted.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_VALUE_PASSTHROUGH=1 "$HAO" "$OUT/svpolicy.arm64e" refuse \
    >"$OUT/passthrough.out" 2>"$OUT/passthrough.err"; rc=$?
if [ $rc = 0 ] && cmp -s "$OUT/native.refuse.out" "$OUT/passthrough.out" &&
   grep -q 'passed through unpatched (HAO_SWIFT_VALUE_PASSTHROUGH)' "$OUT/passthrough.err"; then r=SAME; else r=DIFF; fail=1; fi
echo "passthrough rc=$rc $r"
[ $r = SAME ] || { diff "$OUT/native.refuse.out" "$OUT/passthrough.out" | sed 's/^/    /' | head -8; tail -3 "$OUT/passthrough.err" | sed 's/^/    /'; }
[ $fail = 0 ] && echo "svpolicy: PASS" || echo "svpolicy: FAIL"
exit $fail
