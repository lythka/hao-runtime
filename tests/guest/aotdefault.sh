#!/bin/sh
# hao --aot with no FILE: the default set from the image's lists (aot-list, else stub-identities' fourth field),
# here from HAO_AOT_LIST_DIR (a TEST_SWITCHES build). A path with spaces and a universal file with an x86_64 slice
# are cached, a listed missing file is counted, not failed; a list others can write is not used. A second run skips
# the valid caches; --check reports valid, missing and stale and changes nothing.
#   OUT=dir HAO=/abs/build-ts/hao [T=120] tests/guest/aotdefault.sh
set -u
OUT=${OUT:-$(mktemp -d -t hao-aotdefault)}
HAO=${HAO:?HAO=build-ts/hao}
T=${T:-120}
fail=0
bad() { echo "FAIL $*"; fail=1; }
rm -rf "$OUT/w"; mkdir -p "$OUT/w/bin dir" "$OUT/w/lists" "$OUT/w/c1" "$OUT/w/c2"
chmod 755 "$OUT/w" "$OUT/w/lists" "$OUT/w/c1" "$OUT/w/c2"
W=$(cd "$OUT/w" && pwd -P)
printf 'int main(void){return 0;}\n' > "$W/h.c"
clang -arch arm64e -o "$W/bin dir/one" "$W/h.c"
clang -arch x86_64 -arch arm64 -o "$W/two" "$W/h.c"
codesign -f -s - "$W/bin dir/one" "$W/two" 2>/dev/null
r=${W#/}
printf '# stub identities\naaaa  bbbb  arm64e  %s\naaaa  bbbb  arm64e  %s/missing\naaaa  bbbb  arm64  %s/two\n' "$r/bin dir/one" "$r" "$r" > "$W/lists/stub-identities"
chmod 644 "$W/lists/stub-identities"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c1" > "$OUT/a.log" 2>&1; rc=$?
n=$(ls "$W/c1" | grep -c '\.aot$')
[ $rc = 0 ] && [ "$n" = 2 ] && grep -q '2 programs cached, 0 already valid, 0 failed, 1 listed but not present' "$OUT/a.log" \
    && echo "PASS stub-identities: 2 caches, the missing file counted" || bad "stub-identities rc $rc n $n: $(tail -2 "$OUT/a.log")"
grep -q '\[1/2\]' "$OUT/a.log" && grep -q '\[2/2\]' "$OUT/a.log" && echo "PASS N of M counter" || bad "no [N/M] lines: $(head -3 "$OUT/a.log")"
before=$(ls -l "$W/c1" | md5)
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c1" > "$OUT/a2.log" 2>&1; rc=$?
[ $rc = 0 ] && grep -q '0 programs cached, 2 already valid, 0 failed' "$OUT/a2.log" && [ "$(ls -l "$W/c1" | md5)" = "$before" ] \
    && echo "PASS second run: both caches valid, skipped, nothing written" || bad "second run rc $rc: $(tail -2 "$OUT/a2.log")"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --check --cache "$W/c1" > "$OUT/k1.log" 2>&1; rc=$?
[ $rc = 0 ] && grep -q '2 valid, 0 missing, 0 stale' "$OUT/k1.log" && echo "PASS --check: 2 valid (rc 0)" || bad "--check valid rc $rc: $(tail -2 "$OUT/k1.log")"
one=$(ls "$W/c1" | head -1); cp "$W/c1/$one" "$OUT/one.aot"
printf 'X' | dd of="$W/c1/$one" bs=1 seek=20 conv=notrunc 2>/dev/null
other=$(ls "$W/c1" | sed -n 2p); rm -f "$W/c1/$other"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --check --cache "$W/c1" > "$OUT/k2.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q '0 valid, 1 missing, 1 stale' "$OUT/k2.log" && [ ! -e "$W/c1/$other" ] \
    && echo "PASS --check: a changed cache is stale, a removed one missing (rc 1), nothing written" || bad "--check stale/missing rc $rc: $(tail -3 "$OUT/k2.log")"
timeout -s KILL $T "$HAO" --aot --check --cache "$W/c1" "$W/two" > "$OUT/k3.log" 2>&1; r1=$?
timeout -s KILL $T "$HAO" --aot --check --cache "$OUT" "$W/two" > "$OUT/k4.log" 2>&1; r2=$?
case "$r1:$r2" in 3:3|4:3) echo "PASS --check FILE: rc 3 missing / 4 stale" ;; *) bad "--check FILE rc $r1 $r2: $(cat "$OUT/k3.log" "$OUT/k4.log" | tail -2)" ;; esac
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c1" > "$OUT/a3.log" 2>&1; rc=$?
[ $rc = 0 ] && grep -q '2 programs cached, 0 already valid' "$OUT/a3.log" && echo "PASS rerun rewrites the stale and the missing cache" \
    || bad "rerun rc $rc: $(tail -2 "$OUT/a3.log")"
printf '%s\n' "$W/two" > "$W/lists/aot-list"; chmod 644 "$W/lists/aot-list"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c2" > "$OUT/b.log" 2>&1; rc=$?
[ $rc = 0 ] && [ "$(ls "$W/c2" | grep -c '\.aot$')" = 1 ] && grep -q 'aot-list names' "$OUT/b.log" \
    && echo "PASS aot-list is read first" || bad "aot-list rc $rc: $(tail -2 "$OUT/b.log")"
chmod 666 "$W/lists/aot-list" "$W/lists/stub-identities"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c2" > "$OUT/c.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q 'neither' "$OUT/c.log" && echo "PASS lists others can write are not used (rc 1)" || bad "writable lists rc $rc: $(tail -2 "$OUT/c.log")"
[ $fail = 0 ] && echo "aotdefault: PASS" || { echo "aotdefault: FAIL"; exit 1; }
