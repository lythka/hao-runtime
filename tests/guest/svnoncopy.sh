#!/bin/sh
# A noncopyable native type in the Swift value walk (tests/guest/svnoncopy.swift): the box of a
# Synchronization.Mutex<Int> ("mutex") and of a Mutex<AsyncStream<@Sendable () -> ()>.Continuation>
# ("stream"), value witness flag IsNonCopyable. Under the test switch
# HAO_SWIFT_WALK_FACTS=tests/guest/svnoncopy.facts (a TEST_SWITCHES build) their SAME rows (the second with
# the generated table's key) admit them: equal to native arm64 under JIT, --interp and AOT. Without the
# fixture each is refused.
#   OUT=dir HAO=/abs/build-ts/hao [T=60] tests/guest/svnoncopy.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-svnoncopy)}
HAO=${HAO:-$repo/build-ts/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
for a in arm64e arm64; do
    xcrun swiftc -O -parse-as-library -module-name svnoncopy -target $a-apple-macos27.0 -o "$OUT/svnoncopy.$a" \
        "$here/svnoncopy.swift" 2>"$OUT/build.$a.err" || { cat "$OUT/build.$a.err"; exit 2; }
    codesign -s - -f "$OUT/svnoncopy.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot" "$OUT/svnoncopy.arm64e" >"$OUT/aot.gen" 2>&1 || echo "svnoncopy: aot generation failed"
facts=$here/svnoncopy.facts
fail=0
for part in mutex stream; do
    timeout -s KILL "$T" "$OUT/svnoncopy.arm64" $part >"$OUT/native.$part.out" 2>/dev/null; nrc=$?
    [ $nrc = 0 ] && [ -s "$OUT/native.$part.out" ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 HAO_SWIFT_WALK_FACTS="$facts" "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" HAO_SWIFT_WALK_FACTS="$facts" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/svnoncopy.arm64e" $part >"$OUT/$mode.$part.out" 2>"$OUT/$mode.$part.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/native.$part.out" "$OUT/$mode.$part.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf '%-6s %-7s rc=%s %s\n' $part $mode "$rc" "$r"
        [ $r = SAME ] || tail -4 "$OUT/$mode.$part.err" | cut -c1-300 | sed 's/^/    /'
    done
    # Without the fixture each noncopyable type is refused.
    case $part in mutex) want='size=16 flags=0x3810007 .*type=Synchronization.Mutex<Swift.Int>' ;;
    *) want='size=16 flags=0x3810007 .*type=Synchronization.Mutex<Swift.AsyncStream<@Sendable () -> ()>.Continuation>' ;;
    esac
    timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" "$OUT/svnoncopy.arm64e" $part >"$OUT/nofacts.$part.out" 2>"$OUT/nofacts.$part.err"; rc=$?
    if [ $rc != 0 ] && grep -q "Swift value boundary refused: .*$want" "$OUT/nofacts.$part.err"
    then r=REFUSED; else r=NOT-REFUSED; fail=1; fi
    printf '%-6s nofacts rc=%s %s\n' $part "$rc" "$r"
done
[ $fail = 0 ] && echo "svnoncopy: PASS" || echo "svnoncopy: FAIL"
exit $fail
