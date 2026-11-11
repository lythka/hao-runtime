#!/bin/bash
# The code identity Security.framework gives a listed stub or re-signed native program
# (src/code_identity.c, src/idlist.c; docs/security-model.md, Identity), against Apple's own
# programs on this Mac:
#  - a stub of /usr/bin/true's arm64e slice (hao-embed --host-test) listed in stub-identities:
#    `anchor apple` and `identifier "com.apple.true" and anchor apple` now hold for the stub's
#    file; a requirement the Apple slice itself does not meet still fails;
#  - the same for a running stub of /bin/sleep (SecCodeCopyGuestWithAttributes by pid);
#  - an ad hoc re-signed copy of /usr/bin/true's x86_64 slice listed in native-identities with
#    the original as its reference;
#  - fail closed: unlisted code, a wrong reference cdhash, a list directory writable by group,
#    a chained row; any status other than errSecCSReqFailed (-67050) and no requirement pass
#    through unchanged.
#   OUT=dir [B=build] tests/tools/codeid.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-tools-codeid)}
B=${B:-$repo/build}
fails=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
rm -rf "$OUT"; mkdir -p "$OUT" && chmod 755 "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd -P)
cd "$OUT" || exit 2

clang -arch x86_64 -O1 -Wall -DHAO_TEST_SWITCHES -I"$repo/include" -framework Security -framework CoreFoundation \
    -o codeid_test "$here/codeid_test.c" "$repo/src/code_identity.c" "$repo/src/idlist.c" && codesign -s - -f codeid_test 2>/dev/null || exit 2
cdhash_of() { codesign -dvvv -a "$2" "$1" 2>&1 | sed -n 's/^CDHash=//p'; }
for p in /usr/bin/true /bin/sleep; do
    n=$(basename $p)
    lipo "$p" -thin arm64e -output $n.arm64e || exit 2
    python3 "$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o $n.emb $n.arm64e >embed-$n.log 2>&1 \
        || { echo "codeid: hao-embed $n: $(cat embed-$n.log)" >&2; exit 2; }
done
lipo /usr/bin/true -thin x86_64 -output true.x86 && codesign -s - -f -i com.apple.true true.x86 2>/dev/null || exit 2
lipo /usr/bin/true -thin x86_64 -output other.x86 && codesign -s - -f -i com.example.other other.x86 2>/dev/null || exit 2
S_TRUE=$(cdhash_of true.emb x86_64) R_TRUE=$(cdhash_of true.emb arm64e)
S_SLEEP=$(cdhash_of sleep.emb x86_64) R_SLEEP=$(cdhash_of sleep.emb arm64e)
O_TRUE=$(cdhash_of true.x86 x86_64) X_TRUE=$(cdhash_of /usr/bin/true x86_64)
for h in "$S_TRUE" "$R_TRUE" "$S_SLEEP" "$R_SLEEP" "$O_TRUE" "$X_TRUE"; do [ ${#h} = 40 ] || { echo "codeid: no cdhash ($h)" >&2; exit 2; }; done
rel=${OUT#/}
lists() {  # DIR STUB_ROWS NATIVE_ROWS
    mkdir -p "$1" && chmod 755 "$1"
    printf '# test\n%s' "$2" >"$1/stub-identities"; printf '# test\n%s' "$3" >"$1/native-identities"
    chmod 644 "$1"/*
}
lists L "$S_TRUE  $R_TRUE  arm64e  $rel/true.emb
$S_SLEEP  $R_SLEEP  arm64e  $rel/sleep.emb
" "$O_TRUE  $X_TRUE  x86_64  usr/bin/true
"
lists Lbad "$S_TRUE  0000000000000000000000000000000000000000  arm64e  $rel/true.emb
" ""
lists Lgrp "$S_TRUE  $R_TRUE  arm64e  $rel/true.emb
" ""
chmod 775 Lgrp
lists Lchain "$S_TRUE  $R_TRUE  arm64e  $rel/true.emb
$R_TRUE  $X_TRUE  x86_64  usr/bin/true
" ""

AA='anchor apple'
expect() {  # WHAT WANT(original/decided/errors) ARGS...
    local what=$1 want=$2; shift 2
    local got; got=$(./codeid_test "$@" 2>&1)
    [ "$got" = "$want" ] && pass "$what" || bad "$what: got [$got] want [$want]"
}
ok='original=-67050 decided=0 errors=cleared' no='original=-67050 decided=-67050 errors=kept'
expect "stub file: anchor apple holds for a listed stub" "$ok" "$OUT/L" static true.emb x86_64 "$AA"
expect "stub file: its Apple program's identifier and anchor apple" "$ok" "$OUT/L" static true.emb x86_64 "identifier \"com.apple.true\" and $AA"
expect "stub file: a requirement the Apple slice fails still fails" "$no" "$OUT/L" static true.emb x86_64 "identifier \"com.example.x\" and $AA"
expect "native copy re-signed ad hoc: anchor apple from its listed original" "$ok" "$OUT/L" static true.x86 - "$AA"
expect "unlisted ad hoc code: unchanged" "$no" "$OUT/L" static other.x86 - "$AA"
expect "a wrong reference cdhash: unchanged" "$no" "$OUT/Lbad" static true.emb x86_64 "$AA"
expect "a list directory writable by group is not read" "$no" "$OUT/Lgrp" static true.emb x86_64 "$AA"
expect "a chained row (its reference is listed) is dropped" "$no" "$OUT/Lchain" static true.emb x86_64 "$AA"
expect "another status passes through" "original=-67061 decided=-67061 errors=kept" "$OUT/L" status true.emb x86_64 "$AA" -67061
expect "no requirement: unchanged" "$no" "$OUT/L" status true.emb x86_64 - -67050

# a running stub (hao runs /bin/sleep's arm64e slice)
arch -x86_64 ./sleep.emb 30 & pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ "$(ps -o comm= -p $pid 2>/dev/null)" != "" ] && sleep 0.5 && break; sleep 0.3; done
expect "running stub: anchor apple holds by pid" "$ok" "$OUT/L" pid $pid "$AA"
expect "running stub: identifier com.apple.sleep and anchor apple" "$ok" "$OUT/L" pid $pid "identifier \"com.apple.sleep\" and $AA"
expect "running stub, wrong reference cdhash: unchanged" "$no" "$OUT/Lbad" pid $pid "$AA"
kill $pid 2>/dev/null; wait $pid 2>/dev/null

echo "codeid: $fails failed"
exit $((fails > 0))
