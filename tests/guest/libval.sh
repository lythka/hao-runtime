#!/bin/bash
# Library validation (src/libval.c), guest-image integrity, dyld's rule for
# a dependency that does not load, and DYLD_* injection (tools/hao-embed):
# what a guest may load depends on its own code signature, as natively.
#
# Every hao run is made in each MODES (jit interp aot) and each PATHS:
#   kernel  the kernel holds the guest image's signature (this host)
#   user    HAO_LIBVAL_USER=1: hao decides in user space, as it must on an
#           x86_64 kernel, which holds only 4 KiB-page CodeDirectories
#           (Apple's arm64 code is signed with 16 KiB pages); the signer is
#           Security.framework
#   list    the same with HAO_TRUST_LIST naming the platform library's
#           cdhash (tools/hao-trust-list): the daemon-free signer
# and compared with native arm64:
#  - tests/guest/libval.c signed four ways (ad hoc; hardened runtime;
#    library flag; runtime with disable-library-validation) dlopens an ad hoc
#    universal plug-in (native under hao) and libz, and links an ad hoc
#    arm64-only library weakly (a guest image under hao): native refuses both
#    ad hoc libraries in the runtime and library variants;
#  - plat: links a platform-signed arm64-only system library weakly
#    (PLATLIB, 16 KiB pages): loads in every variant; the user and list paths
#    must name their route in -v;
#  - tampered: the same library with one byte of __text flipped (a copy, the
#    program's load command pointed at it): native dies of SIGKILL
#    (CODESIGNING, Invalid Page), plain and hardened;
#  - strong: the ad hoc library linked non-weakly by a hardened program:
#    native dyld refuses to launch ("Library not loaded", abort); hao must
#    print the same lines and die the same way;
#  - pages: the ad hoc library signed with 4 KiB and with 16 KiB pages, and a
#    tampered 4 KiB one, under plain and hardened programs.
# Then a guest file rewritten in place while hao loads it (tests/guest/
# libvalswap.c, inserted into hao, rewrites it after hao has read it and
# before the image's first segment is mapped), which is what the owner of a
# file can do at any moment: the ad hoc library with a twin that returns
# another value, a user's copy of the platform library with the tampered
# one under a hardened program, the main executable with another program,
# and the library rewritten by the running program itself before its first
# call into it (tests/guest/libvallate.c). What runs must be what was checked.
# Then the embedded form (hao-embed --host-test), and DYLD_INSERT_LIBRARIES
# reaching the embedded program exactly when it reaches the native one.
#   OUT=dir HAO=/abs/build/hao [B=build] [T=60] [MODES="jit interp aot"] [PATHS="kernel user list"]
#   [PLATLIB=arm64-only platform dylib] tests/guest/libval.sh
# OUT must not lie under a group- or world-writable directory (/tmp): the
# list path's HAO_TRUST_LIST is used only from a directory nobody else can write.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
OUT=${OUT:-$(mktemp -d -t hao-libval)}
HAO=${HAO:-$repo/build/hao}
B=${B:-$(dirname "$HAO")}
T=${T:-60}
MODES=${MODES:-"jit interp aot"}
PATHS=${PATHS:-"kernel user list"}
PLATLIB=${PLATLIB:-/System/Library/HIDPlugins/IOHIDEventProcessorFilter.plugin/Contents/MacOS/IOHIDEventProcessorFilter}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
rm -rf "$OUT"/*.out "$OUT"/*.err "$OUT"/*.emb "$OUT/aot" "$OUT/trust"
fail=0
[ -f "$PLATLIB" ] || { echo "FAIL no platform library $PLATLIB (PLATLIB=)"; exit 1; }
set -e
sign() { codesign -s - -f "$@" 2>/dev/null; }
# flip one byte of __text in every arm64 slice of a file
tamper() {
    python3 -I - "$1" <<'EOF'
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
def slices():
    m = struct.unpack('>I', d[:4])[0]
    if m == 0xcafebabe:
        for k in range(struct.unpack('>I', d[4:8])[0]):
            c, s, off, sz, al = struct.unpack('>iiIII', d[8 + 20 * k:28 + 20 * k])
            if c == 0x0100000c: yield off
    elif d[:4] == b'\xcf\xfa\xed\xfe': yield 0
n = 0
for base in slices():
    ncmds = struct.unpack('<I', d[base + 16:base + 20])[0]
    p0 = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack('<II', d[p0:p0 + 8])
        if cmd == 0x19:
            nsect = struct.unpack('<I', d[p0 + 64:p0 + 68])[0]
            for s in range(nsect):
                q = p0 + 72 + 80 * s
                if d[q:q + 16].rstrip(b'\0') == b'__text':
                    off = struct.unpack('<I', d[q + 48:q + 52])[0]
                    d[base + off + 2] ^= 0x01
                    n += 1
        p0 += size
if not n: sys.exit('no __text in ' + p)
open(p, 'wb').write(d)
EOF
}
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -o "$OUT/libnplug.dylib" "$here/libvalplug.c"
clang -arch arm64 -arch arm64e -dynamiclib -O1 -DGUEST -install_name @rpath/libgplug.dylib \
    -o "$OUT/libgplug.dylib" "$here/libvalplug.c"
clang -arch x86_64 -arch arm64 -arch arm64e -dynamiclib -O1 -DINJECT -o "$OUT/libinj.dylib" "$here/libvalplug.c"
sign "$OUT/libnplug.dylib"; sign "$OUT/libgplug.dylib"; sign "$OUT/libinj.dylib"
cat >"$OUT/dlv.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>
EOF
signv() {  # variant file
    case $1 in
    plain) sign "$2" ;;
    runtime) sign -o runtime "$2" ;;
    library) sign -o library "$2" ;;
    dlv) sign -o runtime --entitlements "$OUT/dlv.plist" "$2" ;;
    esac
}
VARIANTS="plain runtime library dlv"
cat >"$OUT/plat.c" <<'EOF'
#include <stdio.h>
extern void *IOHIDEventProcessorFactory(void *, void *) __attribute__((weak_import));
int main(void) { printf("platform library: %s\n", IOHIDEventProcessorFactory ? "loaded" : "missing"); return 0; }
EOF
cat >"$OUT/strong.c" <<'EOF'
#include <stdio.h>
extern int gplug_value(void);
int main(void) { printf("strong libgplug: %d\n", gplug_value()); return 0; }
EOF
cp "$PLATLIB" "$OUT/platt.dylib"; tamper "$OUT/platt.dylib"
for ps in 4096 16384; do
    mkdir -p "$OUT/p$ps"
    cp "$OUT/libgplug.dylib" "$OUT/p$ps/libgplug.dylib"; sign --pagesize $ps "$OUT/p$ps/libgplug.dylib"
done
mkdir -p "$OUT/p4096t"; cp "$OUT/p4096/libgplug.dylib" "$OUT/p4096t/libgplug.dylib"; tamper "$OUT/p4096t/libgplug.dylib"
PROGS=
for a in arm64 arm64e; do
    for v in $VARIANTS; do
        g=$OUT/libval.$v.$a
        clang -arch $a -O1 -o "$g" "$here/libval.c" -Wl,-weak_library,"$OUT/libgplug.dylib" -Wl,-rpath,@executable_path
        signv $v "$g"
    done
    for v in plain runtime; do
        clang -arch $a -O1 -o "$OUT/plat.$v.$a" "$OUT/plat.c" -Wl,-weak_library,"$PLATLIB"; signv $v "$OUT/plat.$v.$a"
        clang -arch $a -O1 -o "$OUT/tampered.$v.$a" "$OUT/plat.c" -Wl,-weak_library,"$PLATLIB"
        install_name_tool -change "$PLATLIB" "$OUT/platt.dylib" "$OUT/tampered.$v.$a" 2>/dev/null; signv $v "$OUT/tampered.$v.$a"
        clang -arch $a -O1 -o "$OUT/strong.$v.$a" "$OUT/strong.c" "$OUT/libgplug.dylib" -Wl,-rpath,@executable_path
        signv $v "$OUT/strong.$v.$a"
        for d in p4096 p16384 p4096t; do
            clang -arch $a -O1 -o "$OUT/$d/pages.$v.$a" "$here/libval.c" -Wl,-weak_library,"$OUT/$d/libgplug.dylib" -Wl,-rpath,@executable_path
            signv $v "$OUT/$d/pages.$v.$a"
        done
    done
done
set +e
mkdir -m 700 -p "$OUT/trust"
echo "$PLATLIB" >"$OUT/trust/paths"
python3 "$repo/tools/hao-trust-list" / "$OUT/trust/paths" >"$OUT/trust/list" 2>"$OUT/trust/err"
grep -q '^[0-9a-f]\{40\} ' "$OUT/trust/list" || { echo "FAIL tools/hao-trust-list did not list $PLATLIB: $(cat "$OUT/trust/err")"; fail=1; }
# hao reads the list once and searches it sorted: put the library's line among 4000 others, out of order,
# in upper case, between comments and lines that are not cdhashes
python3 -I - "$OUT/trust/list" <<'EOF'
import hashlib, sys
p = sys.argv[1]
real = [l for l in open(p).read().splitlines() if l and not l.startswith("#")]
other = [hashlib.sha1(b"other %d" % k).hexdigest() + " other/%d" % k for k in range(4000)]
lines = ["# test list"] + other[:1500] + ["not a cdhash", "", "abcdef"] + [l[:40].upper() + l[40:] for l in real] + other[1500:]
open(p, "w").write("\n".join(lines) + "\n")
EOF
chmod 600 "$OUT/trust/list"
case " $MODES " in *" aot "*)
    mkdir -m 700 "$OUT/aot"
    for f in "$OUT"/libval.*.arm64e "$OUT"/plat.*.arm64e "$OUT"/tampered.*.arm64e "$OUT"/strong.*.arm64e "$OUT"/p*/pages.*.arm64e \
             "$OUT/libgplug.dylib" "$OUT"/p4096/libgplug.dylib "$OUT"/p16384/libgplug.dylib "$PLATLIB"; do
        timeout -s KILL 120 "$HAO" --aot --cache "$OUT/aot" "$f" >/dev/null 2>&1
    done
    ls "$OUT/aot" | grep -q . || { echo "FAIL hao --aot wrote no cache"; fail=1; } ;;
esac

# the dyld lines of a refused launch, without pids, UUIDs and the arch suffix
dyldlines() {
    sed -n 's/^dyld\[[0-9]*\]: /dyld: /p; s/^  Referenced from: .* \(\/.*\)$/  Referenced from: \1/p' "$1" |
        sed 's/\.arm64e$//; s/\.arm64$//'
    grep -o 'not valid for use in process: [^)]*' "$1" | head -1
}
check() {  # label native-out native-rc hao-out hao-rc [native-err hao-err]
    local ok=1
    [ "$3" = "$5" ] && cmp -s "$2" "$4" || ok=0
    if [ $ok = 1 ] && [ $# -ge 7 ] && [ "$3" = 134 ]; then
        [ "$(dyldlines "$6")" = "$(dyldlines "$7")" ] && [ -n "$(dyldlines "$6")" ] || ok=0
    fi
    if [ $ok = 1 ]; then echo "PASS $1 (rc $5)"
    else
        echo "FAIL $1 (rc $5, native $3)"; diff "$2" "$4" | sed 's/^/    /'
        if [ $# -ge 7 ]; then diff <(dyldlines "$6") <(dyldlines "$7") | sed 's/^/    dyld: /'; sed 's/^/    hao stderr: /' "$7" | head -8; fi
        fail=1
    fi
}
# run one program natively and under hao in every mode and path
#   label native-program hao-program route-regex(for -v, "" = none) [args...]
runcase() {
    local label=$1 nat=$2 gst=$3 want=$4; shift 4
    local d; d=$(dirname "$nat")
    (cd "$d" && timeout -s KILL "$T" "$nat" "$@") >"$OUT/$label.native.out" 2>"$OUT/$label.native.err"
    local nrc=$?
    echo "native $label: rc $nrc: $(tr '\n' ';' <"$OUT/$label.native.out")"
    for mode in $MODES; do
        for path in $PATHS; do
            local o=$OUT/$label.$mode.$path env=(env) flag=()
            [ $mode = interp ] && flag=(--interp)
            [ $mode = aot ] && env+=(HAO_CACHE="$OUT/aot") || env+=(HAO_NO_AOT=1)
            case $path in
            user) env+=(HAO_LIBVAL_USER=1) ;;
            list) env+=(HAO_LIBVAL_USER=1 HAO_TRUST_LIST="$OUT/trust/list") ;;
            esac
            (cd "$d" && timeout -s KILL "$T" "${env[@]}" "$HAO" ${flag[@]+"${flag[@]}"} "$gst" "$@") >"$o.out" 2>"$o.err"
            local rc=$?
            check "$label $mode $path" "$OUT/$label.native.out" $nrc "$o.out" $rc "$OUT/$label.native.err" "$o.err"
            if [ -n "$want" ]; then
                local re=$want
                case $path in user) re="user space (HAO_LIBVAL_USER), 16 KiB pages, Security.framework" ;;
                              list) re="user space (HAO_LIBVAL_USER), 16 KiB pages, trusted cdhash list" ;; esac
                (cd "$d" && timeout -s KILL "$T" "${env[@]}" "$HAO" -v ${flag[@]+"${flag[@]}"} "$gst" "$@") 2>&1 | grep -qF "code signature: $re" ||
                    { echo "FAIL $label $mode $path: -v does not name the route '$re'"; fail=1; }
            fi
        done
    done
}
ARGS=("$OUT/libnplug.dylib" /usr/lib/libz.1.dylib)
for v in $VARIANTS; do runcase "$v" "$OUT/libval.$v.arm64" "$OUT/libval.$v.arm64e" "" "${ARGS[@]}"; done
# the runtime and library variants must have refused something natively, or
# the test shows nothing (a host whose dyld stopped validating)
grep -q refused "$OUT/runtime.native.out" && grep -q missing "$OUT/library.native.out" ||
    { echo "FAIL native arm64 refused nothing: library validation is not observable here"; fail=1; }
for v in plain runtime; do
    runcase "plat-$v" "$OUT/plat.$v.arm64" "$OUT/plat.$v.arm64e" "$([ $v = runtime ] && echo "kernel, 16 KiB pages")"
    runcase "tampered-$v" "$OUT/tampered.$v.arm64" "$OUT/tampered.$v.arm64e" ""
    runcase "strong-$v" "$OUT/strong.$v.arm64" "$OUT/strong.$v.arm64e" ""
    for d in p4096 p16384 p4096t; do runcase "$d-$v" "$OUT/$d/pages.$v.arm64" "$OUT/$d/pages.$v.arm64e" ""; done
done
# what native must have done, or the cases show nothing
grep -q loaded "$OUT/plat-runtime.native.out" || { echo "FAIL native: the platform library did not load"; fail=1; }
for v in plain runtime; do
    grep -q . "$OUT/tampered-$v.native.out" && { echo "FAIL native: the tampered library ran ($v)"; fail=1; }
done
grep -q "Library not loaded" "$OUT/strong-runtime.native.err" || { echo "FAIL native: dyld launched strong-runtime"; fail=1; }

# --- a library with two LC_CODE_SIGNATURE commands is not loaded, whatever the policy: one reader, one signature
mkdir -p "$OUT/twosig"
python3 -I - "$OUT/libgplug.dylib" "$OUT/twosig/libgplug.dylib" arm64e <<'EOF'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
n = 0
for k in range(struct.unpack_from(">I", d, 4)[0]):           # every slice of the fat library
    base = struct.unpack_from(">I", d, 8 + 20 * k + 8)[0]
    ncmds, sizeofcmds = struct.unpack_from("<II", d, base + 16)
    o = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, o)
        if cmd == 0x1d: sig = bytes(d[o:o + 16])
        o += size
    assert not any(d[o:o + 16]), "no room in the header padding"
    d[o:o + 16] = sig
    struct.pack_into("<II", d, base + 16, ncmds + 1, sizeofcmds + 16)
    n += 1
assert n
open(dst, "wb").write(d)
EOF
cp "$OUT/libval.plain.arm64e" "$OUT/twosig/prog"
for mode in $MODES; do
    [ $mode = aot ] && continue
    flag=(); [ $mode = interp ] && flag=(--interp)
    got=$(cd "$OUT/twosig" && timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" -v ${flag[@]+"${flag[@]}"} "$OUT/twosig/prog" 2>&1)
    if echo "$got" | grep -q "^weak libgplug: missing" && echo "$got" | grep -q "more than one LC_CODE_SIGNATURE"; then
        echo "PASS library with two LC_CODE_SIGNATURE commands ($mode): not loaded"
    else echo "FAIL library with two LC_CODE_SIGNATURE commands ($mode): $(echo "$got" | grep -i "libgplug" | head -3)"; fail=1; fi
done

# --- a guest file rewritten in place between hao's check and its mapping
RW=$OUT/rw; rm -rf "$RW"; mkdir -p "$RW"
rwok=1
clang -arch x86_64 -O1 -dynamiclib -o "$RW/swap.dylib" "$here/libvalswap.c" && sign "$RW/swap.dylib" || rwok=0
for t in good evil; do
    clang -arch arm64 -arch arm64e -dynamiclib -O1 -DGUEST $([ $t = evil ] && echo -DGVALUE=666) \
        -install_name @rpath/libgplug.dylib -o "$RW/$t.dylib" "$here/libvalplug.c" && sign "$RW/$t.dylib" || rwok=0
    printf '#include <stdio.h>\nint main(void) { puts("main: %s"); return 0; }\n' $t >"$RW/main.$t.c"
    for a in arm64 arm64e; do clang -arch $a -O1 -o "$RW/main.$t.$a" "$RW/main.$t.c" && sign "$RW/main.$t.$a" || rwok=0; done
done
cp "$RW/good.dylib" "$RW/libgplug.dylib"
cat >"$RW/code.c" <<'EOF'
#include <ptrauth.h>
#include <stdio.h>
extern void *IOHIDEventProcessorFactory(void *, void *) __attribute__((weak_import));
int main(void)
{
    if (!IOHIDEventProcessorFactory) { printf("platform library: missing\n"); return 0; }
    const unsigned char *p = ptrauth_strip((const unsigned char *)IOHIDEventProcessorFactory, ptrauth_key_function_pointer);
    printf("platform library: loaded, its first instruction is %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]);
    return 0;
}
EOF
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$RW/strong.$a" "$OUT/strong.c" "$RW/libgplug.dylib" -Wl,-rpath,@executable_path && sign "$RW/strong.$a" || rwok=0
    clang -arch $a -O1 -o "$RW/late.$a" "$here/libvallate.c" "$RW/libgplug.dylib" -Wl,-rpath,@executable_path && sign "$RW/late.$a" || rwok=0
    clang -arch $a -O1 -o "$RW/code.$a" "$RW/code.c" -Wl,-weak_library,"$PLATLIB" &&
        install_name_tool -change "$PLATLIB" "$RW/plat.dylib" "$RW/code.$a" 2>/dev/null && sign -o runtime "$RW/code.$a" || rwok=0
done
[ $rwok = 1 ] || { echo "FAIL building the rewrite cases"; fail=1; }
# Two files hold the same bytes. Read, not mapped: the kernel kills a platform tool (cmp) that maps a file
# whose pages no longer match the signature it holds for that file.
samebytes() { [ "$(cat "$1" | shasum -a 256)" = "$(cat "$2" | shasum -a 256)" ]; }
# swapcase LABEL FILE GOOD EVIL NATIVE-PROGRAM HAO-PROGRAM [PATHS]: FILE starts as GOOD (a new file each run:
# the kernel keeps a file's signature with its vnode) and is rewritten with EVIL while hao loads it. The output
# and status must be the native run's with GOOD, and FILE must hold EVIL afterwards (the rewrite happened).
swapcase() {
    local label=$1 file=$2 good=$3 evil=$4 nat=$5 gst=$6 paths=${7:-$PATHS}
    rm -f "$file"; cp "$good" "$file"
    (cd "$RW" && timeout -s KILL "$T" "$nat") >"$RW/$label.native.out" 2>"$RW/$label.native.err"
    local nrc=$?
    echo "native $label: rc $nrc: $(tr '\n' ';' <"$RW/$label.native.out")"
    for mode in $MODES; do
        [ $mode = aot ] && continue
        for path in $paths; do
            local o=$RW/$label.$mode.$path env=(env HAO_NO_AOT=1) flag=()
            [ $mode = interp ] && flag=(--interp)
            case $path in
            user) env+=(HAO_LIBVAL_USER=1) ;;
            list) env+=(HAO_LIBVAL_USER=1 HAO_TRUST_LIST="$OUT/trust/list") ;;
            esac
            rm -f "$file"; cp "$good" "$file"
            (cd "$RW" && timeout -s KILL "$T" "${env[@]}" DYLD_INSERT_LIBRARIES="$RW/swap.dylib" SWAP_FILE="$file" SWAP_WITH="$evil" \
                "$HAO" ${flag[@]+"${flag[@]}"} "$gst") >"$o.out" 2>"$o.err"
            local rc=$?
            if ! samebytes "$file" "$evil"; then
                echo "FAIL $label $mode $path: the file was not rewritten while hao loaded it (the case shows nothing)"; fail=1
            elif [ $rc = $nrc ] && cmp -s "$RW/$label.native.out" "$o.out"; then
                echo "PASS $label $mode $path: rewritten after the check; what ran is what was checked (rc $rc)"
            else
                echo "FAIL $label $mode $path (rc $rc, native $nrc): $(tr '\n' ';' <"$o.out") $(head -c 300 "$o.err")"; fail=1
            fi
        done
    done
}
if [ $rwok = 1 ]; then
    swapcase rewritten-library "$RW/libgplug.dylib" "$RW/good.dylib" "$RW/evil.dylib" "$RW/strong.arm64" "$RW/strong.arm64e"
    # not the user path: Security.framework refuses a bundle's executable copied out of its bundle (it seals the
    # bundle's Info.plist), so the copy never loads there; the list path is the user-space route for it
    swapcase rewritten-platform-copy "$RW/plat.dylib" "$PLATLIB" "$OUT/platt.dylib" "$RW/code.arm64" "$RW/code.arm64e" \
        "$(echo " $PATHS " | sed 's/ user / /')"
    swapcase rewritten-main "$RW/main" "$RW/main.good.arm64e" "$RW/main.evil.arm64e" "$RW/main.good.arm64" "$RW/main"
    grep -q "strong libgplug: 7" "$RW/rewritten-library.native.out" && grep -q "first instruction" "$RW/rewritten-platform-copy.native.out" ||
        { echo "FAIL native: the rewrite cases' programs did not run"; fail=1; }
    # the running program rewrites its library, then calls into it for the first time (an image built
    # from a copy cannot change; a private mapping of the file kept its bytes too on the kernels tried)
    for mode in $MODES; do
        [ $mode = aot ] && continue
        flag=(); [ $mode = interp ] && flag=(--interp)
        rm -f "$RW/libgplug.dylib"; cp "$RW/good.dylib" "$RW/libgplug.dylib"
        got=$(cd "$RW" && timeout -s KILL "$T" env HAO_NO_AOT=1 "$HAO" ${flag[@]+"${flag[@]}"} "$RW/late.arm64e" "$RW/libgplug.dylib" "$RW/evil.dylib" 2>&1; echo "rc=$?")
        if samebytes "$RW/libgplug.dylib" "$RW/evil.dylib" && [ "$got" = "late libgplug: 7
rc=0" ]; then echo "PASS library rewritten by the running program before its first call ($mode): the loaded code runs"
        else echo "FAIL library rewritten by the running program ($mode): $got"; fail=1; fi
    done
    rm -f "$RW/libgplug.dylib"; cp "$RW/good.dylib" "$RW/libgplug.dylib"
fi

# embedded form: the stub runs the program's own arm64e slice
if [ -x "$repo/tools/hao-embed" ] && [ -f "$B/hao-stub" ]; then
    for v in plain runtime; do
        "$repo/tools/hao-embed" --host-test --stub "$B/hao-stub" -o "$OUT/libval.$v.emb" "$OUT/libval.$v.arm64e" >/dev/null 2>&1 ||
            { echo "FAIL hao-embed $v"; fail=1; continue; }
        (cd /tmp && timeout -s KILL "$T" "$OUT/libval.$v.arm64" "${ARGS[@]}") >"$OUT/$v.native.out" 2>/dev/null
        (cd /tmp && timeout -s KILL "$T" arch -x86_64 "$OUT/libval.$v.emb" "${ARGS[@]}") >"$OUT/$v.emb.out" 2>"$OUT/$v.emb.err"
        check "$v embedded" "$OUT/$v.native.out" 0 "$OUT/$v.emb.out" $?
        n=$(cd /tmp && timeout -s KILL "$T" arch -arm64 -e DYLD_INSERT_LIBRARIES="$OUT/libinj.dylib" "$OUT/libval.$v.arm64" 2>&1 | grep -c INJECTED)
        e=$(cd /tmp && timeout -s KILL "$T" arch -x86_64 -e DYLD_INSERT_LIBRARIES="$OUT/libinj.dylib" "$OUT/libval.$v.emb" 2>&1 | grep -c INJECTED)
        if [ "$n" = "$e" ]; then echo "PASS $v embedded: DYLD_INSERT_LIBRARIES loaded $e time(s), native $n"
        else echo "FAIL $v embedded: DYLD_INSERT_LIBRARIES loaded $e time(s), native $n"; fail=1; fi
    done
else echo "SKIP embedded form (no tools/hao-embed or $B/hao-stub)"; fi
echo "libval: $([ $fail = 0 ] && echo all passed || echo FAILED)"
exit $fail
