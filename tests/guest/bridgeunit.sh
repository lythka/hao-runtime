#!/bin/sh
# Interface units and the typed boundary (src/bridge.c, src/ifunit.c, hao/bridge.h):
# a native helper library typed by a unit keyed by its x86_64 LC_UUID, against
# native arm64.
#   - 5000 bool functions whose x86_64 code leaves 0x5a above al: TYPED 'B'
#     records extend every result (more entry points than the 3072 the old
#     fixed native signature set held);
#   - EXACT records with argument masks: a guest function's address passed as a
#     `long` stays the guest's (the untyped scan replaced it with a thunk), and a
#     callback slot named by the mask still gets its thunk;
#   - a TYPED 'A' record: a 32-byte struct by value through abi.c;
#   - a function reached only through dlsym: typed on its first call (dladdr);
#     printf and snprintf through dlsym go to the interposers their imports bind to;
#   - REFUSED: the call stops with a diagnostic.
# Every hao run (JIT, --interp, AOT) must print what native arm64 prints. Then:
# HAO_BRIDGE_CHECK=1 with a unit whose mask leaves out bh_apply's callback slot
# names it; a unit that fails validation (bad hash, wrong UUID, group-writable)
# is rejected and leaves the library UNKNOWN; HAO_NO_IFUNITS=1 runs untyped
# (informational: the lines that differ are what the unit fixes).
#   OUT=dir HAO=build/hao [T=60] tests/guest/bridgeunit.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-bridgeunit)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
OUT=$(printf '%s' "$OUT" | tr -s /)
rm -rf "$OUT"
mkdir -p "$OUT/if" "$OUT/wrong" "$OUT/bad" "$OUT/aot"
chmod 755 "$OUT" "$OUT/if" "$OUT/wrong" "$OUT/bad"
set -e
python3 -I "$here/bridgeunit_gen.py" "$OUT/src" 5000
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -install_name "$OUT/libbh.dylib" -o "$OUT/libbh.dylib" "$OUT/src/helper.c"
codesign -s - -f "$OUT/libbh.dylib" 2>/dev/null
for a in arm64e arm64; do
    clang -arch $a -O1 -L"$OUT" -lbh -o "$OUT/bridgeunit.$a" "$OUT/src/guest.c"
    codesign -s - -f "$OUT/bridgeunit.$a" 2>/dev/null
done
U=$(python3 -I "$repo/tools/hao_ifunit.py" uuid "$OUT/libbh.dylib")
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/if/$U" --uuid "$U" "$OUT/src/unit.tsv"
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/wrong/$U" --uuid "$U" "$OUT/src/wrong.tsv"
chmod 644 "$OUT/if/$U" "$OUT/wrong/$U"
set +e
fail=0
timeout -s KILL "$T" "$OUT/bridgeunit.arm64" >"$OUT/native.out" 2>"$OUT/native.err"
rc=$?
echo "native rc=$rc lines=$(wc -l <"$OUT/native.out" | tr -d ' ')"
[ $rc = 0 ] || fail=1
HAO_CACHE="$OUT/aot" HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$HAO" --aot "$OUT/bridgeunit.arm64e" >"$OUT/aotgen.out" 2>&1 || fail=1
for mode in jit interp aot; do
    flag=; [ $mode = interp ] && flag=--interp
    cache=$OUT/none; [ $mode = aot ] && cache=$OUT/aot
    HAO_CACHE=$cache HAO_IF_DIR="$OUT/if" HAO_BRIDGE_STATS=1 HAO_VERBOSE=1 timeout -s KILL "$T" "$HAO" $flag "$OUT/bridgeunit.arm64e" \
        >"$OUT/$mode.out" 2>"$OUT/$mode.err"
    rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.out" "$OUT/$mode.out"; then r=SAME; else r=DIFF; fail=1; fi
    if [ $mode = aot ] && ! grep -q "AOT cache .* loaded" "$OUT/$mode.err"; then r="$r (AOT cache not loaded)"; fail=1; fi
    if ! grep -q "interface unit .*$U.*: 5005 records" "$OUT/$mode.err"; then r="$r (unit not loaded)"; fail=1; fi
    echo "$mode   rc=$rc $r  $(grep -o 'bound exact=[0-9]* typed=[0-9]* refused=[0-9]* unknown=[0-9]*' "$OUT/$mode.err")"
    [ "$r" = SAME ] || diff "$OUT/native.out" "$OUT/$mode.out" | sed 's/^/    /' | head -20
    # REFUSED: the guest asks for bh_refused
    (HAO_CACHE=$cache HAO_IF_DIR="$OUT/if" timeout -s KILL "$T" "$HAO" $flag "$OUT/bridgeunit.arm64e" refused \
        >"$OUT/$mode.refused.out" 2>"$OUT/$mode.refused.err") 2>/dev/null
    rc=$?
    if [ $rc = 134 ] && grep -q "bridge: REFUSED call to _bh_refused .*: test: refused by its interface unit" "$OUT/$mode.refused.err" &&
       ! grep -q returned "$OUT/$mode.refused.out"; then r=PASS; else r=FAIL; fail=1; fi
    echo "$mode   refused rc=$rc $r"
done
# HAO_BRIDGE_CHECK: the mask that leaves out bh_apply's callback slot
HAO_IF_DIR="$OUT/wrong" HAO_BRIDGE_CHECK=1 timeout -s KILL "$T" "$HAO" "$OUT/bridgeunit.arm64e" >"$OUT/check.out" 2>"$OUT/check.err"
rc=$?
if grep -q "bridge: CHECK _bh_apply: argument slots 0x1 hold guest code addresses the plan's mask 0 skips" "$OUT/check.err"; then r=PASS; else r=FAIL; fail=1; fi
echo "check  rc=$rc $r  $(grep -c 'bridge: UNKNOWN' "$OUT/check.err") UNKNOWN first-call lines"
# rejected units: each leaves the library UNKNOWN, so the narrow results differ from native
mkdir -p "$OUT/bad/hash" "$OUT/bad/uuid" "$OUT/bad/mode"
python3 -I - "$OUT/if/$U" "$OUT/bad/hash/$U" <<'EOF'
import sys
b = bytearray(open(sys.argv[1], 'rb').read()); b[-2] ^= 1; open(sys.argv[2], 'wb').write(b)
EOF
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/bad/uuid/$U" --uuid 00112233445566778899aabbccddeeff "$OUT/src/unit.tsv"
cp "$OUT/if/$U" "$OUT/bad/mode/$U"
chmod 755 "$OUT/bad/hash" "$OUT/bad/uuid" "$OUT/bad/mode"
chmod 644 "$OUT/bad/hash/$U" "$OUT/bad/uuid/$U"
chmod 664 "$OUT/bad/mode/$U"
for b in hash:"payload hash mismatch" uuid:"library UUID differs" mode:"writable by group or others"; do
    d=${b%%:*}; want=${b#*:}
    HAO_IF_DIR="$OUT/bad/$d" HAO_VERBOSE=1 timeout -s KILL "$T" "$HAO" "$OUT/bridgeunit.arm64e" >"$OUT/bad.$d.out" 2>"$OUT/bad.$d.err"
    if grep -q "interface unit .* rejected: .*$want" "$OUT/bad.$d.err" && ! cmp -s "$OUT/native.out" "$OUT/bad.$d.out"; then r=PASS
    else r=FAIL; fail=1; fi
    echo "reject $d $r"
done
HAO_IF_DIR="$OUT/if" HAO_NO_IFUNITS=1 timeout -s KILL "$T" "$HAO" "$OUT/bridgeunit.arm64e" >"$OUT/nounits.out" 2>&1
echo "HAO_NO_IFUNITS=1 rc=$? $(diff "$OUT/native.out" "$OUT/nounits.out" | grep -c '^>') of $(wc -l <"$OUT/native.out" | tr -d ' ') lines differ (informational)"
[ $fail = 0 ] && echo "bridgeunit: PASS" || echo "bridgeunit: FAIL"
exit $fail
