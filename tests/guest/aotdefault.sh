#!/bin/sh
# hao --aot with no FILE: the default set from the image's lists (aot-list, else stub-identities' fourth field),
# here from HAO_AOT_LIST_DIR (a TEST_SWITCHES build). A path with spaces and a universal file with an x86_64 slice
# are cached, a listed missing file is counted, not failed; a list others can write (mode or ACL), or in a
# directory others can write, is not used. A second run skips the valid caches; --check reports valid, missing and
# stale and changes nothing. --auto (the LaunchDaemon) runs once per libhao: a marker names its generation, and a
# run where a program failed writes none.
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
mkdir -p "$W/c3"; chmod 755 "$W/c3"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c3 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u1.log" 2>&1; r1=$?
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c3 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u2.log" 2>&1; r2=$?
printf 'libhao 00000000000000000000000000000000\n' > "$W/lists/auto-aot.done"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c3 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u3.log" 2>&1; r3=$?
if [ $r1 = 0 ] && [ $r2 = 0 ] && [ $r3 = 0 ] && [ "$(ls "$W/c3" | grep -c '\.aot$')" = 1 ] && grep -q 'done for libhao' "$OUT/u1.log" \
    && grep -q 'done already' "$OUT/u2.log" && grep -q '0 programs cached, 1 already valid' "$OUT/u3.log" \
    && grep -q "^libhao [0-9a-f]\{32\}\$" "$W/lists/auto-aot.done" && ! grep -q '^libhao 0\{32\}' "$W/lists/auto-aot.done"; then
    echo "PASS --auto: caches, then a marker; a second run does nothing; another libhao's marker runs it again (valid caches skipped)"
else bad "--auto rc $r1 $r2 $r3: $(tail -1 "$OUT/u1.log" "$OUT/u2.log" "$OUT/u3.log")"; fi
# a run with a failure leaves no marker (the next start-up retries); a list with an ACL granting write is not used
mkdir -p "$W/c4"; chmod 755 "$W/c4"; printf 'not a Mach-O\n' >"$W/junk"
printf '%s\n%s\n' "$W/two" "$W/junk" >"$W/lists/aot-list"; rm -f "$W/lists/auto-aot.done"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c4 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u4.log" 2>&1; r4=$?
[ $r4 = 1 ] && [ ! -e "$W/lists/auto-aot.done" ] && grep -q 'not marked done' "$OUT/u4.log" && [ "$(ls "$W/c4" | grep -c '\.aot$')" = 1 ] \
    && echo "PASS --auto with a failed program: rc 1, the others cached, no marker" || bad "--auto failure rc $r4: $(tail -2 "$OUT/u4.log"; ls "$W/lists")"
printf '%s\n%s\n' "$W/two" "$W/absent" >"$W/lists/aot-list"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c4 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u5.log" 2>&1; r5=$?
[ $r5 = 1 ] && [ ! -e "$W/lists/auto-aot.done" ] && grep -q 'not marked done' "$OUT/u5.log" \
    && echo "PASS --auto with a listed file not present: rc 1, no marker" || bad "--auto missing rc $r5: $(tail -2 "$OUT/u5.log")"
printf '%s\n' "$W/two" >"$W/lists/aot-list"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c4 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u6.log" 2>&1
chmod 664 "$W/lists/auto-aot.done"; echo stale >"$W/lists/auto-aot.done.tmp"
HAO_AOT_LIST_DIR=$W/lists HAO_CACHE=$W/c4 timeout -s KILL $T "$HAO" --aot --auto > "$OUT/u7.log" 2>&1; r7=$?
[ $r7 = 0 ] && grep -q 'ignored' "$OUT/u7.log" && grep -q 'done for libhao' "$OUT/u7.log" && [ "$(stat -f %Lp "$W/lists/auto-aot.done")" = 644 ] \
    && [ ! -e "$W/lists/auto-aot.done.tmp" ] \
    && echo "PASS --auto: a marker others can write is ignored and replaced; a stale .tmp is not reused" || bad "--auto marker rc $r7: $(tail -3 "$OUT/u7.log")"
chmod +a "everyone allow write" "$W/lists/aot-list" "$W/lists/stub-identities"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c2" > "$OUT/acl.log" 2>&1; ra=$?
chmod -N "$W/lists/aot-list" "$W/lists/stub-identities"
[ $ra = 1 ] && grep -q 'neither' "$OUT/acl.log" && echo "PASS lists whose ACL grants write are not used (rc 1)" || bad "ACL lists rc $ra: $(tail -2 "$OUT/acl.log")"
chmod +a "everyone allow add_file" "$W/lists"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c2" > "$OUT/acld.log" 2>&1; rd=$?
chmod -N "$W/lists"
[ $rd = 1 ] && grep -q 'ACL grants write' "$OUT/acld.log" && echo "PASS a list directory whose ACL grants write is not used (rc 1)" \
    || bad "ACL directory rc $rd: $(tail -2 "$OUT/acld.log")"
chmod 666 "$W/lists/aot-list" "$W/lists/stub-identities"
HAO_AOT_LIST_DIR=$W/lists timeout -s KILL $T "$HAO" --aot --cache "$W/c2" > "$OUT/c.log" 2>&1; rc=$?
[ $rc = 1 ] && grep -q 'neither' "$OUT/c.log" && echo "PASS lists others can write are not used (rc 1)" || bad "writable lists rc $rc: $(tail -2 "$OUT/c.log")"
[ $fail = 0 ] && echo "aotdefault: PASS" || { echo "aotdefault: FAIL"; exit 1; }
