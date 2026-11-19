#!/bin/sh
# A process entitled to a copy-on-write defeatured map must not ask for a memory
# entry of its code cache (xnu kills it on an Intel Mac): JIT is refused, the guest
# runs interpreted with the same output. Without the entitlement JIT stays on.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
LIB=$(cd "${LIB:?}" && pwd)
OUT=${OUT:-$(mktemp -d -t hao-wx-cow)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
printf '#include <stdio.h>\nint main(void){volatile long s=0;for(int i=0;i<1000000;i++)s+=i;printf("sum %%ld\\n",(long)s);return 0;}\n' >"$OUT/g.c"
clang -arch arm64 -mmacosx-version-min=27.0 -O1 -o "$OUT/g" "$OUT/g.c"
codesign -s - -f "$OUT/g"
for v in on off; do
    mkdir -p "$OUT/$v"
    cp "$LIB/hao" "$LIB/libhao.dylib" "$LIB/libhao_identity.dylib" "$OUT/$v/"
    {
        echo '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>'
        echo '<key>com.apple.security.cs.allow-jit</key><true/>'
        [ $v = on ] && echo '<key>com.apple.security.hardened-process.containment.vm.cow-defeatured</key><integer>1</integer>'
        echo '</dict></plist>'
    } >"$OUT/$v.plist"
    codesign -s - -f --entitlements "$OUT/$v.plist" "$OUT/$v/hao"
    timeout -s KILL 60 env -u HAO_FLAGS "$OUT/$v/hao" "$OUT/g" >"$OUT/$v.out" 2>"$OUT/$v.err"
    grep -qx 'sum 499999500000' "$OUT/$v.out"
done
grep -q 'no W^X mapping for translated code' "$OUT/on.err"
! grep -q 'no W^X mapping for translated code' "$OUT/off.err"
echo "wx-cow: PASS"
