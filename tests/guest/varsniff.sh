#!/bin/sh
# Build tests/guest/varsniff.m (+ the native helper dylib varsniffhelper.c,
# universal) for arm64e (guest) and arm64 (reference). Run arm64 natively and
# arm64e under hao with the JIT and with --interp: stdout and the syslog
# LOG_PERROR line on stderr must match native. HAO_NO_VARSNIFF=1 shows the
# old binding (informational). Exit status 0 only if every run matches.
#   OUT=dir HAO=build/hao [T=60] tests/guest/varsniff.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-varsniff}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
LS=/System/Library/PrivateFrameworks/login.framework/Versions/A/Frameworks/loginsupport.framework/Versions/A/loginsupport
mkdir -p "$OUT"
set -e
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -I"$here" -framework CoreFoundation \
    -install_name "$OUT/libvarsniffhelper.dylib" -o "$OUT/libvarsniffhelper.dylib" "$here/varsniffhelper.c"
# link-time stand-in for the private loginsupport (the run binds the system's)
echo 'void DBLoggingLogWithFormat(void){}' > "$OUT/ls.c"
clang -arch arm64 -arch arm64e -dynamiclib -install_name "$LS" -o "$OUT/libloginsupport.dylib" "$OUT/ls.c"
codesign -s - -f "$OUT/libvarsniffhelper.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -fobjc-arc -I"$here" -framework Foundation -L"$OUT" -lvarsniffhelper "$OUT/libloginsupport.dylib" \
        -o "$OUT/varsniff.$a" "$here/varsniff.m"
    codesign -s - -f "$OUT/varsniff.$a" 2>/dev/null
done
set +e
nm -u "$OUT/varsniff.arm64e" | grep -q 'syslog\$DARWIN_EXTSN' && echo "guest imports _syslog\$DARWIN_EXTSN"
run() {  # name cmd...
    n=$1; shift
    timeout -s KILL "$T" "$@" >"$OUT/$n.out" 2>"$OUT/$n.err"
    rc=$?
    grep 'syslog ' "$OUT/$n.err" | sed 's/^.*syslog /syslog /' >>"$OUT/$n.out"
    return $rc
}
run native "$OUT/varsniff.arm64"
rc=$?
echo "native   rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
fail=0
[ $rc = 0 ] || fail=1
for mode in jit interp; do
    flag=; [ $mode = interp ] && flag=--interp
    run $mode "$HAO" $flag "$OUT/varsniff.arm64e"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    echo "$mode $(printf '%-6s' '') rc=$rc $r"
    [ $r = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /'
done
HAO_NO_VARSNIFF=1 run off "$HAO" "$OUT/varsniff.arm64e"
rc=$?
echo "HAO_NO_VARSNIFF=1 rc=$rc, $(diff "$OUT/native.out" "$OUT/off.out" | grep -c '^<') of $(wc -l <"$OUT/native.out" | tr -d ' ') native lines missing or different (informational)"
[ $fail = 0 ] && echo "varsniff: PASS" || echo "varsniff: FAIL"
exit $fail
