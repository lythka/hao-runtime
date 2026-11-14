#!/bin/sh
# Native generic functions whose result is G? (ThrowingTaskGroup, TaskGroup and AsyncStream iterators and next(),
# Dictionary and Set iterators) leave G?'s nil in their own encoding in the guest's storage, 2 where arm64 has 1
# when G's nil sits in a pointer word; hao converts it before guest code reads it (tests/guest/asyncnil.swift).
# The program is built -O and -Onone, arm64 natively and arm64e under hao (JIT, --interp, AOT); every line must
# equal native arm64.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/asyncnil.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-asyncnil)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
fail=0
for opt in O Onone; do
    for a in arm64e arm64; do
        xcrun swiftc -$opt -parse-as-library -target $a-apple-macos27.0 -o "$OUT/asyncnil.$opt.$a" "$here/asyncnil.swift" \
            2>"$OUT/build.$opt.$a.err" || { cat "$OUT/build.$opt.$a.err"; exit 2; }
        codesign -s - -f "$OUT/asyncnil.$opt.$a" 2>/dev/null
    done
    rm -rf "$OUT/aot.$opt"; mkdir -m 700 "$OUT/aot.$opt"
    timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot.$opt" "$OUT/asyncnil.$opt.arm64e" >"$OUT/aot.$opt.gen" 2>&1 ||
        echo "asyncnil: -$opt aot generation failed"
    timeout -s KILL "$T" "$OUT/asyncnil.$opt.arm64" >"$OUT/native.$opt.out" 2>/dev/null; nrc=$?
    echo "-$opt native    rc=$nrc lines=$(wc -l <"$OUT/native.$opt.out" | tr -d ' ')"
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot.$opt" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/asyncnil.$opt.arm64e" >"$OUT/$mode.$opt.out" 2>"$OUT/$mode.$opt.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/native.$opt.out" "$OUT/$mode.$opt.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf -- '-%-5s %-9s rc=%s %s\n' "$opt" "$mode" "$rc" "$r"
        [ $r = SAME ] || diff "$OUT/native.$opt.out" "$OUT/$mode.$opt.out" | sed 's/^/    /' | head -16
    done
done
[ $fail = 0 ] && echo "asyncnil: PASS" || echo "asyncnil: FAIL"
exit $fail
