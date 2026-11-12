#!/bin/sh
# Block chaining is cross-modifying code: the rel32 of an exit jmp is
# rewritten while other threads may run it, so it must be 4-aligned.
#   sites     tests/host/chain.c against libhao: every JIT chain site is
#             4-aligned and gets chained
#   race      8 threads chain the same fresh blocks at once; results right
#   jit-awk   /usr/bin/awk (arm64e slice) under hao --stats: output as
#             native, sites_unaligned=0 link_skips=0
#   aot-awk   hao --aot /usr/bin/awk: every chain site in the blob is
#             4-aligned (found from the exit stubs), and the program run from
#             the blob prints what native awk prints
# Prints PASS/FAIL per check; exit 0 if all pass, 1 otherwise.
#   OUT=dir LIB=build HAO=build/hao [INC=include] tests/host/chain.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-chain)}
LIB=$(cd "${LIB:-$repo/build}" && pwd)
HAO=${HAO:-$LIB/hao}
INC=${INC:-$repo/include}
mkdir -p "$OUT"
T=$OUT/test_chain
clang -arch x86_64 -mmacosx-version-min=27.0 -std=gnu11 -O1 -I"$INC" -o "$T" "$here/chain.c" \
    -L"$LIB" -lhao -Wl,-rpath,"$LIB" || { echo "build failed"; exit 1; }
codesign -s - -f "$T" 2>/dev/null
fail=0
check() {  # name ok(0/1) detail
    if [ "$2" = 1 ]; then r=PASS; else r=FAIL; fail=1; fi
    printf '%-8s %s  %s\n' "$1" "$r" "$3"
}
for m in sites race; do
    timeout -s KILL 120 "$T" $m >"$OUT/$m.out" 2>"$OUT/$m.err"
    rc=$?
    line=$(tail -1 "$OUT/$m.out")
    ok=0; [ $rc = 0 ] && echo "$line" | grep -q ' -> PASS$' && ok=1
    check $m $ok "rc=$rc ${line:-(no summary)}"
done

prog='{n+=length($0)}END{print n}'
in=/usr/share/dict/words
want=$(timeout -s KILL 60 /usr/bin/awk "$prog" "$in")

HAO_NO_AOT=1 timeout -s KILL 300 "$HAO" --stats /usr/bin/awk "$prog" "$in" >"$OUT/jit.out" 2>"$OUT/jit.err"
rc=$?
st=$(grep '^hao-jit: aot_blocks=' "$OUT/jit.err" | tail -1)
ok=0
[ $rc = 0 ] && [ "$(cat "$OUT/jit.out")" = "$want" ] && echo "$st" | grep -q ' sites_unaligned=0 link_skips=0' && ok=1
check jit-awk $ok "rc=$rc out=$(cat "$OUT/jit.out") native=$want ${st#hao-jit: }"

rm -rf "$OUT/aot"; mkdir -p "$OUT/aot"; chmod 700 "$OUT/aot"
timeout -s KILL 300 "$HAO" --aot --cache $OUT/aot /usr/bin/awk >"$OUT/aotgen.out" 2>&1
python3 -I - "$OUT/aot" >"$OUT/aotsites.out" 2>&1 <<'PY'
import glob, struct, sys
# aot file (include/hao/aot.h): hdr | ents[nblocks] | links[nlinks] | code; hdr: magic 8, version, hdr_size,
# uuid 16, gen 16, vmsize, guest_id, nblocks, nlinks, ...
# An exit stub records its site with  lea rcx,[rip+d] ; mov [rbx+0x158],rcx : site = end of the lea + d.
pat = bytes.fromhex("48898b58010000")
files = glob.glob(sys.argv[1] + "/*.aot")
total = bad = cross = notjmp = 0
for p in files:
    b = open(p, "rb").read()
    ver, hs = struct.unpack_from("<II", b, 8)
    nb, nl = struct.unpack_from("<II", b, 64)
    code = b[hs + 8 * nb + 8 * nl:]
    i = code.find(pat)
    while i >= 0:
        if i >= 7 and code[i - 7:i - 4] == bytes.fromhex("488d0d"):
            d = struct.unpack_from("<i", code, i - 4)[0]
            site = i + d
            total += 1
            if site < 1 or code[site - 1] != 0xE9: notjmp += 1
            if site & 3: bad += 1
            if (site & 7) > 4 or (site & 63) > 60: cross += 1
        i = code.find(pat, i + 1)
    print("%s: version=%d" % (p.rsplit("/", 1)[-1], ver))
ok = files and total and not bad and not notjmp
print("blobs=%d sites=%d not_after_E9=%d unaligned4=%d crossing_8_or_64=%d -> %s" % (len(files), total, notjmp, bad, cross, "PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
prc=$?
HAO_CACHE=$OUT/aot timeout -s KILL 300 "$HAO" --stats /usr/bin/awk "$prog" "$in" >"$OUT/aot.out" 2>"$OUT/aot.err"
rc=$?
ab=$(grep '^hao-jit: aot_blocks=' "$OUT/aot.err" | tail -1 | sed -E 's/.*(aot_blocks=[0-9]+).*/\1/')
ok=0
[ $prc = 0 ] && [ $rc = 0 ] && [ "$(cat "$OUT/aot.out")" = "$want" ] && {
    abn=${ab#aot_blocks=}; case $abn in ""|0|*[!0-9]*) ;; *) ok=1;; esac
}
check aot-awk $ok "$(tail -1 "$OUT/aotsites.out"); run rc=$rc out=$(cat "$OUT/aot.out") $ab"
exit $fail
