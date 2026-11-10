#!/bin/bash
# Swift concurrency ladder: build tests/guest/async-*.swift for arm64e, run
# each natively (expected output) and under hao, compare stdout + exit code.
#
#   tests/guest/run-async.sh [-k] [TEST...]     TEST = async-3-actor, 3, or oracle
#   A test containing "hao-lib: async-lib/X.swift" links a universal
#   (x86_64 + arm64e) dylib built from that file: native async code that
#   hao runs natively, calling the translated test code.
#   (no TEST: all async-*.swift plus the modelmanagerdump oracle cases at the end)
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
ORACLE_CASES=1
if [ $# -gt 0 ]; then
    ORACLE_CASES=0
    for a in "$@"; do
        case $a in
        oracle) ORACLE_CASES=1 ;;
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
# Real Swift-concurrency programs (ArgumentParser AsyncParsableCommand):
#   oracle265-modelmanagerdump: macOS 26.5 (25F71) universal modelmanagerdump,
#     thin ad-hoc re-signed slices; expected = the x86_64 slice run natively
#     (under Rosetta here), actual = the arm64e slice under hao.
#   ipsw27-modelmanagerdump: the 27.0 arm64-only tool, same expected text.
# Skipped (not counted) when the files are absent. Select alone with "oracle".
ORACLE265=${ORACLE265:-}
IPSW27=${IPSW27:-}
oracle_case() {   # NAME EXPECTED_CMD... -- HAO_ARGS...
    local name=$1; shift
    local exp=() act=()
    while [ "$1" != "--" ]; do exp+=("$1"); shift; done; shift
    act=("$@")
    (cd "$OUT" && timeout "$TIMEOUT" "${exp[@]}" >"$OUT/$name.native.out" 2>"$OUT/$name.native.err"); local nrc=$?
    (cd "$OUT" && env ${HAO_ENV:-} timeout "$TIMEOUT" "$HAO" "${act[@]}" >"$OUT/$name.hao.out" 2>"$OUT/$name.hao.err"); local hrc=$?
    local why=""
    if [ $hrc -eq 124 ]; then why="timeout ${TIMEOUT}s"
    elif [ $hrc -ne $nrc ]; then why="exit $hrc, native $nrc"
    elif ! cmp -s "$OUT/$name.native.out" "$OUT/$name.hao.out"; then why="stdout differs"
    fi
    if [ -z "$why" ]; then echo "PASS $name"; pass=$((pass + 1)); return; fi
    echo "FAIL $name: $why"; fail=$((fail + 1)); failed+=("$name")
    if [ $QUIET = 0 ]; then
        diff "$OUT/$name.native.out" "$OUT/$name.hao.out" | head -8 | sed 's/^/    /'
        grep -v '^hao: [-<]' "$OUT/$name.hao.err" | head -6 | sed 's/^/    stderr: /'
    fi
}
if [ $ORACLE_CASES = 1 ] && [ -n "$ORACLE265" ] && [ -x "$ORACLE265/modelmanagerdump.x86_64.adhoc" ] && [ -f "$ORACLE265/modelmanagerdump.arm64e.adhoc" ]; then
    oracle_case oracle265-modelmanagerdump arch -x86_64 "$ORACLE265/modelmanagerdump.x86_64.adhoc" -h -- "$ORACLE265/modelmanagerdump.arm64e.adhoc" -h
    if [ -n "$IPSW27" ] && [ -f "$IPSW27/usr/bin/modelmanagerdump" ]; then
        oracle_case ipsw27-modelmanagerdump arch -x86_64 "$ORACLE265/modelmanagerdump.x86_64.adhoc" -h -- "$IPSW27/usr/bin/modelmanagerdump" -h
    fi
fi

echo "async: $pass pass, $fail fail${failed[*]:+ (${failed[*]})}"
[ $fail -eq 0 ]
