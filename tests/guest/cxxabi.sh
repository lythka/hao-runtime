#!/bin/sh
# C++ ABI differences between Apple arm64 and x86_64 at the guest/native
# boundary (tests/guest/cxxabi.cpp: this-return, x8 results, std::string
# layout, std::function size), and the guest-libc++
# cases: the coresymbolicationd and
# systemkeychain crash sites, locales, strings, containers and
# std::function crossing a C++ helper, wchar_t strings, exceptions thrown by
# native libc++ and caught by the guest (and the reverse), RTTI across the
# boundary. Builds cxxabi.cpp for arm64e (guest) and arm64 (reference) against
# libc++ and the helper libcxxabihelper.dylib (x86_64 + arm64 + arm64e; under
# hao it loads as guest code, as an on-disk C++ library with an arm64 slice and
# no ObjC/Swift sections, and libc++ is the arm64e cache's, run as guest code;
# HAO_NATIVE_LIBCXX=1 keeps both native), then runs every section on its own:
# natively, and under hao with the JIT, --interp and an AOT cache. A run
# passes when stdout and the exit status match native arm64.
# Expected failures are listed in EXPECT (the W4 gate; empty since guest libc++,
# P5): the script fails when a section outside EXPECT fails, and when one in
# EXPECT passes in every mode (remove it from EXPECT in the commit that fixes it).
#   OUT=dir HAO=/abs/build/hao [T=30] [SECTIONS="1 2 .."] [EXPECT="2 3 ..."] tests/guest/cxxabi.sh
# Exit status: 0 when the results match EXPECT.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-cxxabi)}
HAO=${HAO:-$repo/build/hao}
T=${T:-30}
SECTIONS=${SECTIONS:-"1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19"}
EXPECT=${EXPECT-""}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)   # the helper's install name is absolute
set -e
clang++ -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -std=c++17 -I"$here" \
    -install_name "$OUT/libcxxabihelper.dylib" -o "$OUT/libcxxabihelper.dylib" "$here/cxxabihelper.cpp"
codesign -s - -f "$OUT/libcxxabihelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang++ -arch $a -O1 -std=c++17 -I"$here" -L"$OUT" -lcxxabihelper -o "$OUT/cxxabi.$a" "$here/cxxabi.cpp"
    codesign -s - -f "$OUT/cxxabi.$a" 2>/dev/null
done
rm -rf "$OUT/aot"; mkdir -m 700 "$OUT/aot"
HAO_CACHE="$OUT/aot" timeout -s KILL 120 "$HAO" --aot "$OUT/cxxabi.arm64e" >/dev/null 2>&1
set +e
bad=0
for s in $SECTIONS; do
    ( timeout -s KILL "$T" "$OUT/cxxabi.arm64" $s >"$OUT/s$s.native.out" 2>"$OUT/s$s.native.err"; exit $? ) 2>/dev/null
    nrc=$?
    line="s$s native rc=$nrc"
    nfail=0
    for mode in jit interp aot; do
        case $mode in
        jit) set -- "$HAO" ;;
        interp) set -- "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot" "$HAO" ;;
        esac
        ( timeout -s KILL "$T" "$@" "$OUT/cxxabi.arm64e" $s >"$OUT/s$s.$mode.out" 2>"$OUT/s$s.$mode.err"; exit $? ) 2>/dev/null
        rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/s$s.native.out" "$OUT/s$s.$mode.out"; then r=SAME; else r=DIFF; nfail=$((nfail + 1)); fi
        line="$line | $mode rc=$rc $r"
    done
    case " $EXPECT " in *" $s "*) exp=1 ;; *) exp=0 ;; esac
    if [ $nfail != 0 ] && [ $exp = 0 ]; then line="$line  NEW FAILURE"; bad=$((bad + 1)); fi
    if [ $nfail = 0 ] && [ $exp = 1 ]; then line="$line  NOW PASSES (remove from EXPECT)"; bad=$((bad + 1)); fi
    [ $nfail != 0 ] && line="$line
    native: $(head -c 160 "$OUT/s$s.native.out" | LC_ALL=C tr '\n' ' ')
    hao:    $(head -c 160 "$OUT/s$s.jit.out" | LC_ALL=C tr '\n' ' ')$(grep -m1 -i 'signal\|abort\|hao:' "$OUT/s$s.jit.err" | head -c 120)"
    echo "$line"
done
[ $bad = 0 ] && echo "cxxabi: PASS (expected failures: ${EXPECT:-none})" || echo "cxxabi: FAIL ($bad unexpected)"
exit $bad
