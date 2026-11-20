#!/bin/sh
# The Swift call map's callback direction (src/swiftcall.c hao_swiftcall_callback): native code calls guest
# witnesses of a native protocol's requirements (tests/guest/swiftcallcb_lib.swift) whose direct values have a
# nil whose encoding differs by architecture: a result (String?, String?)? in x0-x3 / rax rdx rcx r8 and an
# argument in x0-x3 / rdi rsi rdx rcx, the outer nil's word 1 being 1 on arm64 and 2 on x86_64. The library is
# universal (library evolution: its requirement descriptors are exported) and loaded natively; the requirements' rows (by requirement descriptor, ...Tq), keyed by its x86_64
# LC_UUID, and the type's case images are a fixture (test switch HAO_SWIFT_WALK_FACTS, a TEST_SWITCHES build).
# With the rows every hao run (JIT, --interp, AOT) must print what native arm64 prints; without them (CTL,
# default build/hao) the run must differ. A REFUSE row is refused at the witness's first call.
#   OUT=dir HAO=/abs/build-ts/hao [CTL=/abs/build/hao] [T=60] tests/guest/swiftcallcb.sh
set -u
export HAO_NATIVE_APP_LIBS=1   # the library's x86_64 slice runs natively
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftcallcb)}
HAO=${HAO:-$repo/build-ts/hao}
CTL=${CTL:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT"
d=$(cd "$OUT" && pwd)
key=SSSg5title_AA8subtitletSg
z=0000000000000000
fail=0
mkdir -p "$d/aot" && chmod 700 "$d/aot"
for t in x86_64 arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -emit-library -module-name SCBLib -enable-library-evolution \
        -emit-module -emit-module-path "$d/mod-$t/SCBLib.swiftmodule" \
        -Xlinker -install_name -Xlinker "$d/libSCBLib.dylib" -o "$d/lib-$t.dylib" "$here/swiftcallcb_lib.swift" \
        2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
done
lipo -create "$d/lib-x86_64.dylib" "$d/lib-arm64.dylib" "$d/lib-arm64e.dylib" -output "$d/libSCBLib.dylib"
codesign -s - -f "$d/libSCBLib.dylib" 2>/dev/null
for t in arm64 arm64e; do
    swiftc -target $t-apple-macos14 -O -parse-as-library -I "$d/mod-$t" -L"$d" -lSCBLib -o "$d/swiftcallcb.$t" \
        "$here/swiftcallcb.swift" 2>"$d/build.err" || { cat "$d/build.err"; exit 2; }
    codesign -s - -f "$d/swiftcallcb.$t" 2>/dev/null
done
U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$d/libSCBLib.dylib")
sym() { nm -arch x86_64 "$d/libSCBLib.dylib" | grep -oE "_\\\$s6SCBLib$1\$" | sed 's/^_//' | head -1; }
labels=$(sym '6SourceP6labels[A-Za-z0-9_]*FTq'); show=$(sym '6SourceP4show[A-Za-z0-9_]*FTq')
[ -n "$labels" ] && [ -n "$show" ] || { echo "requirement descriptors not found"; exit 2; }
# the requirements' rows (self a class reference in x20 / r13) and the type's case images
facts() {
    cat <<EOT
type $key CONVERT
image $key 0 $z$z$z$z $z$z$z$z $z$z$z$z $z$z$z$z
image $key 1 ${z}0100000000000000$z$z ${z}ffffffffffffffff$z$z ${z}0200000000000000$z$z ${z}ffffffffffffffff$z$z
call $labels $U METHOD_DESCRIPTOR CONVERT
value r $key o $1 x0:rax x1:rdx x2:rcx x3:r8
value p0 Si g SAME x0:rdi
value s 0 g SAME x20:r13
call $show $U METHOD_DESCRIPTOR CONVERT
value r SS o SAME x0:rax x1:rdx
value p0 $key g CONVERT x0:rdi x1:rsi x2:rdx x3:rcx
value s 0 g SAME x20:r13
EOT
}
facts CONVERT >"$d/rows.facts"
facts REFUSE >"$d/refuse.facts"
HAO_SWIFT_WALK_FACTS="$d/rows.facts" timeout -s KILL "$T" "$HAO" --aot --cache "$d/aot" "$d/swiftcallcb.arm64e" >/dev/null 2>&1 ||
    echo "aot generation failed"
timeout -s KILL "$T" "$d/swiftcallcb.arm64" >"$d/native.out" 2>/dev/null; nrc=$?
echo "native    rc=$nrc lines=$(wc -l <"$d/native.out" | tr -d ' ')"
[ $nrc = 0 ] && [ "$(wc -l <"$d/native.out" | tr -d ' ')" = 6 ] || fail=1
for mode in jit interp aot; do
    case $mode in
    jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
    interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
    aot) set -- env HAO_CACHE="$d/aot" "$HAO" ;;
    esac
    HAO_SWIFT_WALK_FACTS="$d/rows.facts" timeout -s KILL "$T" "$@" "$d/swiftcallcb.arm64e" >"$d/$mode.out" 2>"$d/$mode.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf '%-9s rc=%s %s\n' $mode "$rc" "$r"
    [ $r = SAME ] || { diff "$d/native.out" "$d/$mode.out" | head -8; tail -3 "$d/$mode.err" | cut -c1-300; } | sed 's/^/    /'
done
# Without the rows the nils cross unconverted: the run must differ (the case the rows exist for).
if [ -x "$CTL" ]; then
    timeout -s KILL "$T" env HAO_NO_AOT=1 "$CTL" "$d/swiftcallcb.arm64e" >"$d/control.out" 2>"$d/control.err"; rc=$?
    if [ $rc = $nrc ] && cmp -s "$d/native.out" "$d/control.out"; then r=SAME; fail=1; else r=DIFF; fi
    printf '%-9s rc=%s %s (without rows)\n' control "$rc" "$r"
fi
# A REFUSE row: refused at the labels witness's first call, before anything is printed.
HAO_SWIFT_WALK_FACTS="$d/refuse.facts" timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" "$d/swiftcallcb.arm64e" \
    >"$d/refuse.out" 2>"$d/refuse.err"; rc=$?
if [ $rc != 0 ] && [ ! -s "$d/refuse.out" ] &&
   grep -q "Swift value boundary refused: a value the call map refuses (call=$labels " "$d/refuse.err"; then r=REFUSED
else r=NOT-REFUSED; fail=1; fi
printf '%-9s rc=%s %s\n' refuse "$rc" "$r"
[ $fail = 0 ] && echo "swiftcallcb: PASS" || echo "swiftcallcb: FAIL"
exit $fail
