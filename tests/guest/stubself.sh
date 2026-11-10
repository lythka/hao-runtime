#!/bin/bash
# An embedded program's stub and its own file (src/main.c hao_self_main): the ARM
# slice is loaded only from the file the kernel is running, the re-exec that sets
# the ObjC tagged-pointer switch never runs another file, and a process whose
# credentials changed at exec does not re-exec at all.
#   OUT=dir [B=build] [T=60] tests/guest/stubself.sh
# The executable's path is made to name another program between the kernel's exec
# and the stub's main by a library inserted with DYLD_INSERT_LIBRARIES
# (tests/guest/stubswap.c), which repoints the symbolic link the program was
# started through: what the owner of a link in an exec's path can do at any
# moment, here at a fixed one. The guest (tests/guest/stubself.c) says which
# program it is and whether its process was re-exec'd.
# Prints PASS/FAIL per case; exit 1 if any failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/hao-stubself}
B=${B:-$repo/build}
T=${T:-60}
rm -rf "$OUT"; mkdir -p "$OUT/emb" "$OUT/sg"
cd "$OUT" || exit 2
fails=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# A and B: no ObjC, so their stubs are marked __hao_noreexec (tools/hao-embed);
# Ao and Bo link Foundation, so their stubs re-exec for the tagged-pointer switch
for w in A B; do
    clang -arch arm64e -O1 -DWHO="\"$w\"" -o "$w" "$here/stubself.c" && codesign -s - -f "$w" 2>/dev/null || exit 2
    clang -arch arm64e -O1 -DWHO="\"$w\"" -framework Foundation -o "${w}o" "$here/stubself.c" && codesign -s - -f "${w}o" 2>/dev/null || exit 2
    for p in "$w" "${w}o"; do
        python3 "$repo/tools/hao-embed" --stub "$B/hao-stub" --host-test -o "emb/$p" "$p" >/dev/null || exit 2
    done
done
clang -arch x86_64 -O1 -dynamiclib -o swap.dylib "$here/stubswap.c" && codesign -s - -f swap.dylib 2>/dev/null || exit 2

# run NAME ARGS...: stdout+stderr in NAME.out, exit status in rc
run() { local name=$1; shift; timeout -s KILL "$T" "$@" >"$name.out" 2>&1; rc=$?; }
# swapped NAME LINK TO: start LINK's program with the library that repoints LINK to TO before main
swapped() {
    run "$1" arch -x86_64 /bin/bash -c 'DYLD_INSERT_LIBRARIES="$1" SWAP_LINK="$2" SWAP_TO="$3" exec "$2"' - \
        "$OUT/swap.dylib" "$2" "$3"
}
has() { grep -q -- "$2" "$1.out"; }

# --- controls: the probe tells the programs and the re-exec apart
run a arch -x86_64 emb/A
[ $rc = 7 ] && has a "^stubself A$" && has a "^re-exec: no$" && pass "no ObjC: runs in the process it was started in" ||
    bad "no ObjC: rc $rc: $(head -c 300 a.out)"
run ao arch -x86_64 emb/Ao
[ $rc = 7 ] && has ao "^stubself A$" && has ao "^re-exec: yes$" && has ao "^tags: plain$" &&
    pass "ObjC: one re-exec, tagged pointers plain" || bad "ObjC: rc $rc: $(head -c 300 ao.out)"
ln -s emb/A link0
run l0 arch -x86_64 ./link0
[ $rc = 7 ] && has l0 "^stubself A$" && pass "through a symbolic link: runs" || bad "through a symbolic link: rc $rc: $(head -c 300 l0.out)"

# --- the path names another program by the time the stub opens it
ln -s "$OUT/emb/A" link1
swapped s1 "$OUT/link1" "$OUT/emb/B"
if [ "$(readlink link1)" != "$OUT/emb/B" ]; then bad "the link was not repointed (inserted library not run): $(head -c 300 s1.out)"
elif [ $rc = 126 ] && has s1 "no longer names the file this process runs" && ! has s1 "^stubself"; then
    pass "link repointed before the stub opens its file: refused (rc 126), the other program's ARM code not loaded"
else bad "link repointed before the open: rc $rc: $(head -c 300 s1.out)"; fi

# --- the same where a sandbox denies the process its own proc_pidinfo: the file
# is then known from the pair the kernel passed at exec (apple[] executable_file=).
# Under that sandbox this host's `arch` dies of SIGTRAP and the JIT's code of
# SIGILL, with or without hao's own-file check, so the program is started by an
# x86_64 launcher (spawn0.c prints how it ended) and interpreted (HAO_FLAGS=i).
clang -arch x86_64 -O1 -o spawn0 "$here/spawn0.c" && codesign -s - -f spawn0 2>/dev/null || exit 2
printf '(version 1)\n(allow default)\n(deny process-info-pidinfo)\n' >nopidinfo.sb
run p0 env HAO_FLAGS=i sandbox-exec -f nopidinfo.sb "$OUT/spawn0" "$OUT/emb/A"
has p0 "^exit 7$" && has p0 "^stubself A$" && has p0 "^pidinfo: no$" &&
    pass "proc_pidinfo denied: runs (the file is known from the exec's executable_file=)" ||
    bad "proc_pidinfo denied: $(head -c 300 p0.out)"
ln -s "$OUT/emb/A" link1p
run p1 env HAO_FLAGS=i sandbox-exec -f nopidinfo.sb /usr/bin/env DYLD_INSERT_LIBRARIES="$OUT/swap.dylib" \
    SWAP_LINK="$OUT/link1p" SWAP_TO="$OUT/emb/B" "$OUT/spawn0" "$OUT/link1p"
if [ "$(readlink link1p)" != "$OUT/emb/B" ]; then bad "the link was not repointed (inserted library not run): $(head -c 300 p1.out)"
elif has p1 "^exit 126$" && has p1 "no longer names the file this process runs" && ! has p1 "^stubself"; then
    pass "proc_pidinfo denied, link repointed before the open: refused (rc 126)"
else bad "proc_pidinfo denied, link repointed before the open: $(head -c 300 p1.out)"; fi

# --- the path names another program by the time the stub re-execs
ln -s "$OUT/emb/Ao" link2
swapped s2 "$OUT/link2" "$OUT/emb/Bo"
if [ "$(readlink link2)" != "$OUT/emb/Bo" ]; then bad "the link was not repointed (inserted library not run): $(head -c 300 s2.out)"
elif [ $rc = 7 ] && has s2 "^stubself A$" && has s2 "^re-exec: yes$" && has s2 "^tags: plain$" && ! has s2 "^stubself B$"; then
    pass "link repointed before the re-exec: the stub re-execs its own file, not the link's new target"
else bad "link repointed before the re-exec: rc $rc: $(head -c 300 s2.out)"; fi

# --- the loop guard
id=$(stat -f '%d:%i' emb/Ao)
run g1 env HAO_SELF="$id" arch -x86_64 emb/Ao
[ $rc = 7 ] && has g1 "^tags: obfuscated$" && has g1 "tagged pointers are still obfuscated" &&
    pass "HAO_SELF naming this file, tagged pointers obfuscated: no second re-exec, and it says so" ||
    bad "planted loop guard: rc $rc: $(head -c 400 g1.out)"
run g2 env HAO_SELF="1:1" arch -x86_64 emb/Ao
[ $rc = 7 ] && has g2 "^tags: plain$" && ! has g2 "obfuscated" && pass "HAO_SELF naming another file: ignored, re-exec as usual" ||
    bad "stale loop guard: rc $rc: $(head -c 400 g2.out)"

# --- credentials changed at exec: no re-exec. A set-group-ID copy, the group one
# this user is in but not the primary one, so egid != gid without root.
grp=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
if [ -n "$grp" ] && cp emb/Ao sg/Ao && chgrp "$grp" sg/Ao && chmod g+s sg/Ao && [ -g sg/Ao ]; then
    run sg arch -x86_64 sg/Ao
    if [ $rc = 7 ] && has sg "^stubself A$" && has sg "^ids: changed$" && has sg "^re-exec: no$"; then
        pass "set-group-ID embedded program (egid $grp): no re-exec ($(grep '^tags:' sg.out))"
    else bad "set-group-ID embedded program: rc $rc: $(head -c 400 sg.out)"; fi
    ln -s "$OUT/sg/Ao" link3
    run sgl arch -x86_64 ./link3
    [ $rc = 7 ] && has sgl "^stubself A$" && has sgl "^re-exec: no$" && pass "set-group-ID embedded program through a symbolic link: runs, no re-exec" ||
        bad "set-group-ID through a link: rc $rc: $(head -c 400 sgl.out)"
else echo "SKIP set-group-ID embedded program (no second group, or the file system refuses the bit)"; fi
echo "stubself: $fails failed"
exit $((fails != 0))
