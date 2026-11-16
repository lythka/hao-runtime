#!/bin/sh
# Swift `dynamic` functions: each one's prologue passes its own address to the native runtime's
# swift_getFunctionReplacement, which compares it with the function's chain entry. Handing it a native
# entry point for the guest function made every such function call itself forever
# (tests/guest/dynfunc.swift). The program is built -O and -Onone, arm64 natively and arm64e under hao
# (JIT, --interp, AOT); every line must equal native arm64. Then again under an interface unit for the native
# libswiftCore whose rows mark both arguments as code (argmask 0x3, as the generated 26B131 unit does): the
# runtime's rule for these two functions comes before the unit's mask.
#   OUT=dir HAO=/abs/build/hao [T=60] tests/guest/dynfunc.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-dynfunc)}
HAO=${HAO:-$repo/build/hao}
T=${T:-60}
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd)
fail=0
for opt in O Onone; do
    for a in arm64e arm64; do
        xcrun swiftc -$opt -parse-as-library -target $a-apple-macos27.0 -o "$OUT/dynfunc.$opt.$a" "$here/dynfunc.swift" \
            2>"$OUT/build.$opt.$a.err" || { cat "$OUT/build.$opt.$a.err"; exit 2; }
        codesign -s - -f "$OUT/dynfunc.$opt.$a" 2>/dev/null
    done
    rm -rf "$OUT/aot.$opt"; mkdir -m 700 "$OUT/aot.$opt"
    timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/aot.$opt" "$OUT/dynfunc.$opt.arm64e" >"$OUT/aot.$opt.gen" 2>&1 ||
        echo "dynfunc: -$opt aot generation failed"
    timeout -s KILL "$T" "$OUT/dynfunc.$opt.arm64" >"$OUT/native.$opt.out" 2>/dev/null; nrc=$?
    echo "-$opt native    rc=$nrc lines=$(wc -l <"$OUT/native.$opt.out" | tr -d ' ')"
    [ $nrc = 0 ] || fail=1
    for mode in jit interp aot; do
        case $mode in
        jit) set -- env HAO_NO_AOT=1 "$HAO" ;;
        interp) set -- env HAO_NO_AOT=1 "$HAO" --interp ;;
        aot) set -- env HAO_CACHE="$OUT/aot.$opt" "$HAO" ;;
        esac
        timeout -s KILL "$T" "$@" "$OUT/dynfunc.$opt.arm64e" >"$OUT/$mode.$opt.out" 2>"$OUT/$mode.$opt.err"; rc=$?
        if [ $rc = $nrc ] && cmp -s "$OUT/native.$opt.out" "$OUT/$mode.$opt.out"; then r=SAME; else r=DIFF; fail=1; fi
        printf -- '-%-5s %-9s rc=%s %s\n' "$opt" "$mode" "$rc" "$r"
        [ $r = SAME ] || diff "$OUT/native.$opt.out" "$OUT/$mode.$opt.out" | sed 's/^/    /' | head -16
    done
done
printf '#include <dlfcn.h>\n#include <mach-o/dyld.h>\n#include <mach-o/loader.h>\n#include <stdio.h>\n#include <string.h>\nint main(void){dlopen("/usr/lib/swift/libswiftCore.dylib",RTLD_NOW);for(unsigned i=0;i<_dyld_image_count();i++){const char*n=_dyld_get_image_name(i);if(!n||!strstr(n,"/libswiftCore.dylib"))continue;const struct mach_header_64*h=(const void*)_dyld_get_image_header(i);const struct load_command*c=(const void*)(h+1);for(unsigned k=0;k<h->ncmds;k++){if(c->cmd==LC_UUID){const unsigned char*u=((const struct uuid_command*)c)->uuid;for(int j=0;j<16;j++)printf("%%02x",u[j]);printf("\\n");return 0;}c=(const void*)((const char*)c+c->cmdsize);}}return 1;}\n' >"$OUT/swiftcore_uuid.c"
clang -arch x86_64 -o "$OUT/swiftcore_uuid" "$OUT/swiftcore_uuid.c" && U=$("$OUT/swiftcore_uuid") || { echo "dynfunc: no libswiftCore UUID"; exit 2; }
mkdir -p "$OUT/if"
printf '_swift_getFunctionReplacement\tEXACT\t-\t0x3\t-\t-\tC_CC\n_swift_getOrigOfReplaceable\tEXACT\t-\t0x1\t-\t-\tC_CC\n' >"$OUT/unit.tsv"
python3 -I "$repo/tools/hao_ifunit.py" write "$OUT/if/$U" --uuid "$U" "$OUT/unit.tsv" || exit 2
for opt in O Onone; do
    HAO_IF_DIR="$OUT/if" HAO_NO_AOT=1 timeout -s KILL "$T" "$HAO" "$OUT/dynfunc.$opt.arm64e" >"$OUT/unit.$opt.out" 2>"$OUT/unit.$opt.err"; rc=$?
    if [ $rc = 0 ] && cmp -s "$OUT/native.$opt.out" "$OUT/unit.$opt.out"; then r=SAME; else r=DIFF; fail=1; fi
    printf -- '-%-5s %-9s rc=%s %s\n' "$opt" "unit" "$rc" "$r"
done
[ $fail = 0 ] && echo "dynfunc: PASS" || echo "dynfunc: FAIL"
exit $fail
