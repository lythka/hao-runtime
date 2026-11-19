#!/bin/sh
# The Swift value walk's per-type facts and the survey image's audit (tests/guest/svfacts.swift). Under the
# test switch HAO_SWIFT_WALK_FACTS=tests/guest/svfacts.facts (a TEST_SWITCHES build) a row's verdict
# replaces the walk's rules for its type: "same" (a SAME row on InlineArray, which the rules refuse) and
# "convert" (a CONVERT row's case images) must equal native arm64 under JIT, --interp and AOT, "refuse" (a
# REFUSE row on Int?, which the rules certify) must be refused. Without the fixture "same" is refused. Under
# HAO_SWIFT_VALUE_AUDIT=1 "refuse" goes on, equal to native, with one hao-swift-audit record naming the
# refused type by its table key, its parent chain and the guest caller's image UUID and offset; a release
# build (REL) ignores the audit and HAO_SWIFT_VALUE_STRICT, says so once, and under HAO_SWIFT_VALUE_REPORT=1
# reports "same" (report-only).
# Under HAO_SWIFT_KEYS=1 the "same" lookup is one hao-swift-key record with the row's answer.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svfacts.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svfacts)}
HAO=${HAO:-$repo/build-ts/hao}
# Uncertified values are refused here, as they are by default only once their rows land (a test build).
export HAO_SWIFT_VALUE_STRICT=1
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -module-name svfacts -target $a-apple-macos27.0 -o "$OUT/svfacts.$a" \
        "$here/svfacts.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svfacts.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svfacts.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svfacts: aot generation failed"
facts=$here/svfacts.facts
fail=0
for part in same convert refuse; do
    timeout -s KILL "$T" "$OUT/svfacts.arm64" $part >"$OUT/native.$part.out" 2>/dev/null; nrc=$?
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" HAO_SWIFT_WALK_FACTS="$facts" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/svfacts.arm64e" $part >"$OUT/$mode.$part.out" 2>"$OUT/$mode.$part.err"; rc=$?
        if [ $part = refuse ]; then
            if [ $rc != 0 ] && [ ! -s "$OUT/$mode.$part.out" ] &&
               grep -q 'Swift value boundary refused: .*type=Swift.Optional<Swift.Int>' "$OUT/$mode.$part.err"; then r=REFUSED
            else r=NOT-REFUSED; fail=1; fi
        elif [ $rc = $nrc ] && cmp -s "$OUT/native.$part.out" "$OUT/$mode.$part.out"; then r=SAME
        else r=DIFF; fail=1; fi
        printf '%-8s %-7s rc=%s %s\n' $part $mode "$rc" "$r"
        case $r in SAME|REFUSED) ;; *) tail -4 "$OUT/$mode.$part.err" | cut -c1-300 | sed 's/^/    /' ;; esac
    done
done
# Without the fixture, the rules alone refuse the InlineArray.
timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" "$OUT/svfacts.arm64e" same >"$OUT/nofacts.out" 2>"$OUT/nofacts.err"; rc=$?
if [ $rc != 0 ] && grep -q 'Swift value boundary refused: ' "$OUT/nofacts.err"; then r=REFUSED; else r=NOT-REFUSED; fail=1; fi
echo "same     nofacts rc=$rc $r"
# The audit: the refused value goes on; one record names Int? (its table key), its chain and the guest
# caller's image UUID and offset, and no address or path.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" HAO_SWIFT_VALUE_AUDIT=1 "$HAO" "$OUT/svfacts.arm64e" refuse \
    >"$OUT/audit.out" 2>"$OUT/audit.err"; rc=$?
n=$(grep -cE '^hao-swift-audit \{"v":1,.*"class":"known-wrong","type":"SiSg","kind":514,.*"depth":1,"chain":\["SiSg_7svfacts1CCSgSgt"\],.*"caller_image":"[0-9A-F]{32}","caller_offset":[0-9]+,' "$OUT/audit.err")
# no address or path in a record: image UUIDs and offsets only
grep '^hao-swift-audit' "$OUT/audit.err" | grep -qE '"lr"|"pc"|/' && n=0
if [ $rc = 0 ] && cmp -s "$OUT/native.refuse.out" "$OUT/audit.out" && [ "$n" = 1 ] &&
   ! grep -q 'Swift value boundary refused' "$OUT/audit.err"; then r=RECORDED; else r=NOT-RECORDED; fail=1; fi
echo "refuse   audit   rc=$rc $r ($n record)"
[ $r = RECORDED ] || grep -a 'hao-swift-audit' "$OUT/audit.err" | head -3 | cut -c1-400 | sed 's/^/    /'
# The keys: under HAO_SWIFT_KEYS each type the walk looked up is one hao-swift-key record, once, with the
# fixture's answer (the InlineArray's SAME row), where its metadata lies and no address or path; --stats counts.
timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" HAO_SWIFT_KEYS=1 "$HAO" --stats "$OUT/svfacts.arm64e" same \
    >"$OUT/keys.out" 2>"$OUT/keys.err"; rc=$?
n=$(grep -cE '^hao-swift-key \{"v":1,"process":"svfacts.arm64e","key":"s11InlineArrayVy\$1_SiG","row":"same","where":"(cache|heap)",' "$OUT/keys.err")
d=$(grep '^hao-swift-key ' "$OUT/keys.err" | sed 's/"x86_cache":"[0-9A-F]*"//' | sort | uniq -d | wc -l | tr -d ' ')
grep '^hao-swift-key ' "$OUT/keys.err" | grep -qE '0x|/' && n=0
if [ $rc = 0 ] && cmp -s "$OUT/native.same.out" "$OUT/keys.out" && [ "$n" = 1 ] && [ "$d" = 0 ] &&
   grep -qE '^hao-swift-keys: met=[0-9]+ same=1 ' "$OUT/keys.err"; then r=RECORDED; else r=NOT-RECORDED; fail=1; fi
echo "same     keys    rc=$rc $r"
[ $r = RECORDED ] || grep -a 'hao-swift-key' "$OUT/keys.err" | head -4 | cut -c1-300 | sed 's/^/    /'
# A release build (REL, default build/hao) ignores the audit and the strict switch and says so once; under
# HAO_SWIFT_VALUE_REPORT=1 it reports the uncertified value, which goes on equal to native.
REL=${REL:-$repo/build/hao}
if [ -x "$REL" ]; then
    timeout -s KILL "$T" env HAO_NO_AOT=1 HAO_SWIFT_VALUE_AUDIT=1 HAO_SWIFT_VALUE_REPORT=1 "$REL" "$OUT/svfacts.arm64e" same >"$OUT/release.out" 2>"$OUT/release.err"; rc=$?
    if [ $rc = 0 ] && [ "$(grep -c 'hao_swift_value_audit ignored: this libhao is not a survey build' "$OUT/release.err")" = 1 ] &&
       cmp -s "$OUT/native.same.out" "$OUT/release.out" && ! grep -q 'Swift value boundary refused' "$OUT/release.err" &&
       grep -q 'not certified, passed as found (report-only)' "$OUT/release.err"
    then r=IGNORED; else r=NOT-IGNORED; fail=1; fi
    echo "same     release audit rc=$rc $r"
fi
[ $fail = 0 ] && echo "svfacts: PASS" || echo "svfacts: FAIL"
exit $fail
