#!/bin/bash
# Swift concurrency ladder: build tests/guest/async-*.swift for arm64e, run
# each natively (expected output) and under hao, compare stdout + exit code.
#
#   tests/guest/run-async.sh [-k] [TEST...]     TEST = async-3-actor or 3
#   A test containing "hao-lib: async-lib/X.swift" links a universal
#   (x86_64 + arm64e) dylib built from that file: native async code that
#   hao runs natively, calling the translated test code.
#   (no TEST: all async-*.swift)
#
# Env: HAO (default build/hao), SWIFTFLAGS (default -Onone), TIMEOUT (s, 20),
#      OUT (build dir, default build/guest-async), HAO_ENV ("HAO_INTERP=1 ...",
#      extra environment for the hao run), RUNS (repeat each hao run N times).
# -k keeps going quietly; failures print the first lines of the diff and of
# hao's stderr. Last line: "async: N pass, M fail".
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
HAO=${HAO:-$ROOT/build/hao}
OUT=${OUT:-$ROOT/build/guest-async}
TIMEOUT=${TIMEOUT:-20}
SWIFTFLAGS=${SWIFTFLAGS:--Onone}
RUNS=${RUNS:-1}
QUIET=0
[ "${1:-}" = "-k" ] && { QUIET=1; shift; }
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)   # the runs cd into OUT

tests=()
if [ $# -gt 0 ]; then
    for a in "$@"; do
        case $a in
        [0-9]*) for f in "$HERE"/async-"$a"-*.swift; do tests+=("$(basename "$f" .swift)"); done ;;
        *) tests+=("$(basename "$a" .swift)") ;;
        esac
    done
else
    for f in "$HERE"/async-*.swift; do tests+=("$(basename "$f" .swift)"); done
fi

pass=0; fail=0; failed=()
for t in ${tests[@]+"${tests[@]}"}; do
    src=$HERE/$t.swift bin=$OUT/$t
    # "hao-lib: PATH" in the test: a Swift library (PATH relative to this
    # directory) built as a universal x86_64 + arm64e dylib; the guest runs
    # its arm64e slice natively and its x86_64 slice under hao.
    lib=$(sed -n 's/.*hao-lib: \([^ ]*\.swift\).*/\1/p' "$src" | head -1)
    libflags=()
    if [ -n "$lib" ]; then
        mod=$(basename "$lib" .swift) libsrc=$HERE/$lib dylib=$OUT/lib$mod.dylib
        if [ ! -f "$dylib" ] || [ "$libsrc" -nt "$dylib" ]; then
            rm -f "$dylib"
            for arch in x86_64 arm64e; do
                xcrun swiftc -target $arch-apple-macos27.0 $SWIFTFLAGS -parse-as-library -emit-library -module-name "$mod" \
                    -emit-module -emit-module-path "$OUT/$mod.$arch/$mod.swiftmodule" \
                    -Xlinker -install_name -Xlinker "$dylib" -o "$OUT/lib$mod.$arch.dylib" "$libsrc" || break
            done >"$OUT/$mod.build.log" 2>&1
            if [ -f "$OUT/lib$mod.x86_64.dylib" ] && [ -f "$OUT/lib$mod.arm64e.dylib" ] &&
               lipo -create -output "$dylib" "$OUT/lib$mod.x86_64.dylib" "$OUT/lib$mod.arm64e.dylib" 2>>"$OUT/$mod.build.log" &&
               codesign -s - -f "$dylib" 2>>"$OUT/$mod.build.log"; then
                rm -f "$bin"
            else
                rm -f "$dylib"
                echo "FAIL $t (build $mod: $(grep -m1 error "$OUT/$mod.build.log"))"; fail=$((fail + 1)); failed+=("$t"); continue
            fi
        fi
        libflags=(-I "$OUT/$mod.arm64e" -L "$OUT" -l"$mod")
    fi
    if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ]; then
        if ! xcrun swiftc -target arm64e-apple-macos27.0 $SWIFTFLAGS ${libflags[@]+"${libflags[@]}"} -o "$bin" "$src" 2>"$OUT/$t.build.log" ||
           ! codesign -s - -f "$bin" 2>>"$OUT/$t.build.log"; then
            echo "FAIL $t (build: $(head -1 "$OUT/$t.build.log"))"; fail=$((fail + 1)); failed+=("$t"); continue
        fi
    fi
    (cd "$OUT" && timeout "$TIMEOUT" "$bin" >"$OUT/$t.native.out" 2>"$OUT/$t.native.err"); nrc=$?
    verdict=PASS why=""
    for ((r = 1; r <= RUNS; r++)); do
        (cd "$OUT" && env ${HAO_ENV:-} timeout "$TIMEOUT" "$HAO" "$bin" >"$OUT/$t.hao.out" 2>"$OUT/$t.hao.err"); hrc=$?
        if [ $hrc -eq 124 ]; then why="timeout ${TIMEOUT}s"
        elif [ $hrc -ne $nrc ]; then why="exit $hrc, native $nrc"
        elif ! cmp -s "$OUT/$t.native.out" "$OUT/$t.hao.out"; then why="stdout differs"
        fi
        [ -n "$why" ] && { verdict=FAIL; why="$why (run $r/$RUNS)"; break; }
    done
    if [ $verdict = PASS ]; then
        echo "PASS $t"; pass=$((pass + 1))
    else
        echo "FAIL $t: $why"; fail=$((fail + 1)); failed+=("$t")
        if [ $QUIET = 0 ]; then
            diff "$OUT/$t.native.out" "$OUT/$t.hao.out" | head -8 | sed 's/^/    /'
            grep -v '^hao: [-<]' "$OUT/$t.hao.err" | head -6 | sed 's/^/    stderr: /'
        fi
    fi
done
echo "async: $pass pass, $fail fail${failed[*]:+ (${failed[*]})}"
[ $fail -eq 0 ]
