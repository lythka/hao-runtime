#!/bin/sh
# Swift metadata entry points (src/swiftmeta.c): build tests/guest/swiftthunks.swift
# for arm64e (guest) and arm64 (reference), twice: as swiftc builds it by default, and with layout-string value
# witnesses (the 27.0 system builds use them: enum tag functions called by the runtime's layout-string walkers).
# Run arm64 natively and arm64e under hao with the JIT, --interp and an AOT cache. Each hao run must print exactly
# what native arm64 prints, and no native->guest call may take the NX-fault path ("hao: nx faults: 0" in the --stats
# report). A last JIT run with HAO_NO_SWIFT_THUNKS=1 must print the same through the fault path: it shows the cases
# reach guest code through Swift metadata (0 faults there means the test lost its coverage, which fails too).
# Then the same for tests/guest/swiftentry.swift, whose @main entry point (__TEXT,__swift5_entry) a native helper
# reads from the main executable's header and calls, as ExtensionFoundation does for an .appex.
#   OUT=dir HAO=build/hao [T=60] [SWIFTFLAGS=-Onone] tests/guest/swiftthunks.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-swiftthunks}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
SWIFTFLAGS=${SWIFTFLAGS:--Onone}
LS="-enable-experimental-feature LayoutStringValueWitnesses -enable-experimental-feature LayoutStringValueWitnessesInstantiation
    -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation"
mkdir -p "$OUT"
fail=0
# check V D PROG: D/PROG.arm64 natively, D/PROG.arm64e under hao in each mode
check() {
    v=$1 d=$2 prog=$3
    rm -rf "$d/aot"; mkdir -m 700 "$d/aot"
    HAO_CACHE="$d/aot" timeout -s KILL "$T" "$HAO" --aot "$d/$prog.arm64e" >"$d/aot.gen" 2>&1 || echo "$v: aot generation failed"
    timeout -s KILL "$T" "$d/$prog.arm64" >"$d/native.out" 2>"$d/native.err"
    rc=$?
    echo "$v native        rc=$rc lines=$(wc -l <"$d/native.out" | tr -d ' ')"
    [ $rc = 0 ] || fail=1
    for mode in jit interp aot nothunks; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" --stats ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --stats --interp ;;
        aot) set -- env HAO_CACHE="$d/aot" "$HAO" --stats ;;
        nothunks) set -- env HAO_NO_AOT=1 HAO_NO_SWIFT_THUNKS=1 "$HAO" --stats ;;
        esac
        timeout -s KILL "$T" "$@" "$d/$prog.arm64e" >"$d/$mode.out" 2>"$d/$mode.err"
        rc=$?
        nx=$(sed -n 's/^hao: nx faults: \([0-9]*\) native->guest.*/\1/p' "$d/$mode.err" | tail -1)
        sm=$(grep -m1 '^hao-swiftmeta: ' "$d/$mode.err" | sed 's/^hao-swiftmeta: images=[0-9]* //; s/ bad_layout.*resume_parent/ resume_parent/; s/ task_entry_unmade.*//; s/|//')
        r=SAME
        [ $rc = 0 ] && cmp -s "$d/native.out" "$d/$mode.out" || r=DIFF
        if [ $mode = nothunks ]; then
            [ "${nx:-0}" -gt 0 ] || r="$r(no fault path: coverage lost)"
        else
            [ "${nx:-x}" = 0 ] || r="$r(nx faults ${nx:-?})"
        fi
        [ "$r" = SAME ] || fail=1
        printf '%-6s %-9s rc=%s %s nx=%s %s\n' "$v" "$mode" "$rc" "$r" "${nx:-?}" "$sm"
        [ "$r" = SAME ] || { diff "$d/native.out" "$d/$mode.out" | head -8
            grep '^hao\|^objc\[' "$d/$mode.err" | grep -v '^hao: \(blocks\|threads\|bridge\)\|^hao-' | head -8; } | sed 's/^/    /'
    done
}
for v in plain layout; do
    flags=$SWIFTFLAGS; [ $v = layout ] && flags="$SWIFTFLAGS $LS"
    d=$OUT/$v; mkdir -p "$d"
    for a in arm64e arm64; do
        swiftc $flags -parse-as-library -target $a-apple-macos27.0 -o "$d/swiftthunks.$a" "$here/swiftthunks.swift" || { echo "$v: build failed"; exit 1; }
        codesign -s - -f "$d/swiftthunks.$a" 2>/dev/null
    done
    check $v "$d" swiftthunks
done
# The program's entry point (__TEXT,__swift5_entry), read from the main executable's header and called by native
# code as ExtensionFoundation does (swiftentryhelper.c)
d=$OUT/entry; mkdir -p "$d"
clang -arch x86_64 -arch arm64 -arch arm64e -mmacosx-version-min=27.0 -O1 -dynamiclib -install_name "$d/libswiftentryhelper.dylib" \
    -o "$d/libswiftentryhelper.dylib" "$here/swiftentryhelper.c" && codesign -s - -f "$d/libswiftentryhelper.dylib" 2>/dev/null ||
    { echo "entry: helper build failed"; exit 1; }
for a in arm64e arm64; do
    swiftc $SWIFTFLAGS -parse-as-library -target $a-apple-macos27.0 -o "$d/swiftentry.$a" "$here/swiftentry.swift" \
        -Xlinker "$d/libswiftentryhelper.dylib" || { echo "entry: build failed"; exit 1; }
    codesign -s - -f "$d/swiftentry.$a" 2>/dev/null
done
check entry "$d" swiftentry
grep -q '^entered again through __swift5_entry (2)$' "$d/native.out" || { echo "entry: the native run did not reach the entry point"; fail=1; }
[ $fail = 0 ] && echo "swiftthunks: PASS" || echo "swiftthunks: FAIL"
exit $fail
