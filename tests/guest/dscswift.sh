#!/bin/sh
# Swift metadata of arm64e-only shared-cache libraries (guest images, src/dsc.c) whose relative indirect references
# go through slots of the cache's uniqued GOTs, outside every segment of the image, that no code of the image loads:
# "\x02" symbolic references in mangled names, conformance ProtocolRefs/TypeRefs and resilient-witness requirements.
# Native libswiftCore follows them (sharingd: ResolveAsSymbolicReference on AutoUnlockDaemon's field types).
# tests/guest/dsc/swiftgot.c, built per library around one exported type descriptor, derives everything at run time
# from the image's sections (see its header) in three runs:
#   count   the outside-image references (must be non-zero: the case exercises the class)
#   fields  every non-generic type's metadata and every field type by mangled name in the type's context
#   conf    every conformance record: protocol, type, swift_conformsToProtocol
#   keypath key path patterns in __TEXT,__const whose external component's property descriptor is reached through
#           such a slot (no metadata names a pattern: hao binds them in its swift_getKeyPath trap); case repair only,
#           the count must be non-zero
# Built for arm64 (run natively as the reference) and arm64e (under hao: JIT, --interp, AOT, HAO_NO_SWIFT_THUNKS=1,
# HAO_NO_JIT_MEMORY=1: binding is data, it must hold in every mode); every hao run must
# print what native prints, and hao -v must say it loaded the library from the arm64e shared cache (fail closed:
# a host where the library has an x86_64 build, or no arm64e cache, does not exercise the class).
# The link-time stand-in (tests/guest/dsc/swiftgot_stub.c) carries the install name and is never installed.
#   OUT=dir HAO=/abs/build/hao [T=60] [CASES="rdma connmode ulpn repair"] [MODES="jit interp aot nothunks nojit"]
#   tests/guest/dscswift.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$here/dsc
OUT=${OUT:-${TMPDIR:-/tmp}/hao-dscswift}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
CASES=${CASES:-rdma connmode ulpn repair}
MODES=${MODES:-jit interp aot nothunks nojit}
PF=/System/Library/PrivateFrameworks
lib() {
    case $1 in
    rdma) echo "$PF/AppleThunderboltRDMADetection.framework/Versions/A/AppleThunderboltRDMADetection"
          echo '_$s29AppleThunderboltRDMADetection0bC0CMn' ;;
    connmode) echo "$PF/ConnectedMode.framework/Versions/A/ConnectedMode"
          echo '_$s13ConnectedMode7AdapterCMn' ;;
    ulpn) echo "$PF/ULPNHeuristicsClientFramework.framework/Versions/A/ULPNHeuristicsClientFramework"
          echo '_$s29ULPNHeuristicsClientFramework0A0CMn' ;;
    repair) echo "$PF/CoreRepairUI.framework/CoreRepairUI"
          echo '_$s12CoreRepairUI26PartsAndServiceHistoryViewVMa' ;;
    *) return 1 ;;
    esac
}
mkdir -p "$OUT"
cd "$OUT" || exit 1
fail=0
for c in $CASES; do
    info=$(lib $c) || { echo "$c: unknown case"; fail=1; continue; }
    path=$(echo "$info" | sed -n 1p)
    anchor=$(echo "$info" | sed -n 2p)
    ok=1
    for a in arm64 arm64e; do
        clang -arch $a -dynamiclib -DANCHOR="\"$anchor\"" -install_name "$path" -o "lib$c-stub.$a.dylib" \
            "$src/swiftgot_stub.c" &&
        clang -arch $a -O1 -DANCHOR="\"$anchor\"" -o "$c.$a" "$src/swiftgot.c" "lib$c-stub.$a.dylib" \
            -L/usr/lib/swift -lswiftCore -lobjc &&
        codesign -s - -f "$c.$a" 2>/dev/null || ok=0
    done
    [ $ok = 1 ] || { echo "$c: build failed"; fail=1; continue; }
    rm -rf aot.$c; mkdir -m 700 aot.$c
    HAO_CACHE="$OUT/aot.$c" timeout -s KILL "$T" "$HAO" --aot "./$c.arm64e" >/dev/null 2>&1
    case $c in repair) runs="count keypath" ;; *) runs="count fields conf" ;; esac
    for w in $runs; do
        timeout -s KILL "$T" ./$c.arm64 $w >$c.$w.native.out 2>$c.$w.native.err
        rc=$?
        printf '%s\n' "$c $w native rc=$rc lines=$(wc -l <$c.$w.native.out | tr -d ' ') | $(tail -1 $c.$w.native.out)"
        [ $rc = 0 ] || fail=1
        if [ $w = count ] && ! grep -q 'outside-image slots: .*[1-9]' $c.$w.native.out; then
            echo "$c: no outside-image references: the case does not exercise the class"; fail=1
        fi
        if [ $w = keypath ] && ! grep -q 'outside-image slot: [1-9]' $c.$w.native.out; then
            echo "$c: no key path pattern through an outside-image slot: the case does not exercise the class"; fail=1
        fi
        for mode in $MODES; do
            case $mode in
            jit) set -- env HAO_NO_AOT=1 "$HAO" -v ;;
            interp) set -- env HAO_NO_AOT=1 "$HAO" -v --interp ;;
            aot) set -- env HAO_CACHE="$OUT/aot.$c" "$HAO" -v ;;
            nothunks) set -- env HAO_NO_AOT=1 HAO_NO_SWIFT_THUNKS=1 "$HAO" -v ;;
            nojit) set -- env HAO_NO_AOT=1 HAO_NO_JIT_MEMORY=1 "$HAO" -v ;;
            *) echo "unknown mode $mode"; fail=1; continue ;;
            esac
            timeout -s KILL "$T" "$@" ./$c.arm64e $w >$c.$w.$mode.out 2>$c.$w.$mode.err
            rc=$?
            if [ $rc = 0 ] && cmp -s $c.$w.native.out $c.$w.$mode.out; then r=SAME; else r=DIFF; fail=1; fi
            grep -q "mapped $path from the arm64e shared cache" $c.$w.$mode.err || { r="$r NOT-FROM-CACHE"; fail=1; }
            echo "$c $w $mode rc=$rc $r"
            case $r in SAME) ;; *)
                diff $c.$w.native.out $c.$w.$mode.out | head -12 | sed 's/^/    /'
                grep -m1 'host fault\|fatal error\|Fatal' $c.$w.$mode.err | cut -c1-240 | sed 's/^/    /' ;;
            esac
        done
    done
done
[ $fail = 0 ] && echo "dscswift: PASS" || echo "dscswift: FAIL"
exit $fail
