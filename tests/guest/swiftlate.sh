#!/bin/sh
# Categories on Swift classes the Swift runtime realizes after image
# registration (tests/guest/swiftlate.swift, guest dylib swiftlatelib.swift):
# classes with a generic superclass, missing from __objc_classlist, with
# @objc extensions in __objc_catlist. Built for arm64 (reference, native) and
# arm64e (under hao: JIT, --interp, AOT); each section and the whole program
# must print what native arm64 prints, exit 0, leave no libobjc warning, and
# (under hao) report deferred categories attached in the --stats line (the
# whole program: every one of them; a section: those of the class it uses).
# It first checks that the build has the shape: the classes absent from the
# class list and named by categories.
#   OUT=dir HAO=/abs/build/hao [T=60] [MODES="jit interp aot"] tests/guest/swiftlate.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-swiftlate)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
MODES=${MODES:-jit interp aot}
SECTIONS="instance byname local"
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    mkdir -p "$OUT/$a"
    swiftc -O -target $a-apple-macos27.0 -parse-as-library -emit-library -module-name swiftlatelib \
        -emit-module -emit-module-path "$OUT/$a/swiftlatelib.swiftmodule" \
        -Xlinker -install_name -Xlinker @rpath/libswiftlatelib.dylib -o "$OUT/$a/libswiftlatelib.dylib" \
        "$here/swiftlatelib.swift"
    swiftc -O -target $a-apple-macos27.0 -module-name swiftlate -I "$OUT/$a" -L "$OUT/$a" -lswiftlatelib \
        -Xlinker -rpath -Xlinker @executable_path -o "$OUT/$a/swiftlate" "$here/swiftlate.swift"
    codesign -s - -f "$OUT/$a/libswiftlatelib.dylib" "$OUT/$a/swiftlate" 2>/dev/null
done
set +e
# shape: categories whose class is not in the class list
shape=$(python3 - "$OUT/arm64e/swiftlate" <<'EOF'
import subprocess, sys
out = subprocess.run(['xcrun', 'dyld_info', '-fixups', sys.argv[1]], capture_output=True, text=True).stdout
fx, cl, cats = {}, set(), []
for l in out.splitlines():
    p = l.split()
    if len(p) >= 5 and p[2].startswith('0x'):
        fx[int(p[2], 16)] = p[4]
        if p[1] == '__objc_classlist': cl.add(int(p[4], 16))
        if p[1] == '__objc_catlist': cats.append(int(p[4], 16))
n = sum(1 for c in cats if (c + 8) in fx and fx[c + 8].startswith('0x') and int(fx[c + 8], 16) not in cl)
print(n)
EOF
)
echo "categories on classes outside the class list: $shape"
[ "${shape:-0}" -ge 3 ] || { echo "swiftlate: the build does not have the shape"; exit 1; }
case " $MODES " in *" aot "*)
    rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
    HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/arm64e/swiftlate" "$OUT/arm64e/libswiftlatelib.dylib" \
        >"$OUT/aot.log" 2>&1 || echo "aot: --aot failed: $(tail -1 "$OUT/aot.log")" ;;
esac
fail=0
for s in all $SECTIONS; do
    arg=$s; [ $s = all ] && arg=
    timeout -s KILL "$T" "$OUT/arm64/swiftlate" $arg >"$OUT/native.$s.out" 2>"$OUT/native.$s.err" || fail=1
done
echo "native lines=$(wc -l <"$OUT/native.all.out" | tr -d ' ')"
for s in all $SECTIONS; do
    arg=$s; [ $s = all ] && arg=
    printf '%-9s' $s
    for m in $MODES; do
        case $m in
        jit) set -- env HAO_NO_AOT=1 "$HAO" --stats ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --stats --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" --stats ;;
        esac
        o=$OUT/$m.$s
        timeout -s KILL "$T" "$@" "$OUT/arm64e/swiftlate" $arg >"$o.out" 2>"$o.err"
        rc=$?
        att=$(sed -n 's/^hao-objc: .*deferred_categories=\([0-9]*\) attached=\([0-9]*\).*/\1:\2/p' "$o.err" | tail -1)
        d=${att%%:*}; a=${att##*:}
        if [ $rc = 0 ] && cmp -s "$OUT/native.$s.out" "$o.out" && ! grep -q '^objc\[' "$o.err" &&
           [ -n "$att" ] && [ "$a" -gt 0 ] && { [ $s != all ] || [ "$d" = "$a" ]; }; then r=SAME; else r=DIFF; fail=1; fi
        printf ' %-6s rc=%-4s cats=%-6s' $r $rc "${att:--}"
        [ $r = SAME ] || { diff "$OUT/native.$s.out" "$o.out" | head -8; grep '^objc\[\|^hao\[' "$o.err" | head -4; } \
            | sed 's/^/    /' >"$o.why"
    done
    echo
    for m in $MODES; do [ -s "$OUT/$m.$s.why" ] && { echo "  $m:"; cat "$OUT/$m.$s.why"; }; rm -f "$OUT/$m.$s.why"; done
done
[ $fail = 0 ] && echo "swiftlate: PASS" || echo "swiftlate: FAIL"
exit $fail
