#!/bin/sh
# GSS-API struct layout across the native boundary (src/gss.c). gss_OID_desc,
# gss_OID_set and channel bindings are laid out differently on x86_64
# (pack(2)) and arm64; a native OID reaching guest code, or a guest OID
# reaching native code, must be converted. Builds tests/guest/gss.c for
# arm64e (guest) and arm64 (reference), runs arm64 natively and arm64e under
# hao with the JIT, --interp, AOT and HAO_NO_JIT_MEMORY=1, and diffs every
# transcript against native arm64. Without the fix the native
# gss_test_oid_set_member reads a guest OID's elements pointer from the wrong
# offset and faults in memcmp (the 27.0 sshd-auth SIGSEGV).
#   OUT=dir HAO=build/hao [T=60] tests/guest/gss.sh      exit 1 on any mismatch
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-gss)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT"
set -e
for a in arm64e arm64; do
    clang -arch $a -O1 -framework GSS -o "$OUT/gss.$a" "$here/gss.c"
    codesign -s - -f "$OUT/gss.$a" 2>/dev/null
done
set +e
fail=0
bad() { echo "FAIL $1: $2"; fail=1; }

timeout -s KILL "$T" "$OUT/gss.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
nrc=$?
echo "native   rc=$nrc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ "$nrc" = 0 ] || { echo "native run failed"; cat "$OUT/native.err"; exit 1; }
grep -q '^done' "$OUT/native.out" || { echo "native transcript is incomplete"; exit 1; }
grep -q 'present=1' "$OUT/native.out" || echo "note: native reports krb5 mech absent (still a valid control)"

mkdir -p "$OUT/cache"; chmod 755 "$OUT/cache"; rm -f "$OUT/cache/"*
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/cache" "$OUT/gss.arm64e" >/dev/null 2>&1

for m in jit interp nojit aot; do
    case $m in
        jit)    env="" ; args="" ;;
        interp) env="" ; args="--interp" ;;
        nojit)  env="HAO_NO_JIT_MEMORY=1" ; args="" ;;
        aot)    env="HAO_CACHE=$OUT/cache" ; args="" ;;
    esac
    timeout -s KILL "$T" env $env "$HAO" $args "$OUT/gss.arm64e" >"$OUT/$m.out" 2>"$OUT/$m.err"
    rc=$?
    echo "$m   rc=$rc lines=$(wc -l <"$OUT/$m.out" | tr -d ' ')"
    [ "$rc" = "$nrc" ] || bad $m "exit status $rc, native $nrc"
    cmp -s "$OUT/native.out" "$OUT/$m.out" || { bad $m "transcript differs from native arm64"; diff "$OUT/native.out" "$OUT/$m.out" | head -20; }
done
[ $fail = 0 ] && echo "gss: PASS" || echo "gss: FAIL"
exit $fail
