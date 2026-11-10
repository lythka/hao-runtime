#!/bin/bash
# Regenerate the fuzz seed corpora from the small sources in tests/fuzz/seeds/
# (clang-built, so no Apple binaries are involved). 4 KiB segment alignment
# keeps the Mach-O seeds a few KiB, so mutations land on structure.
#   tests/fuzz/make_seeds.sh [OUTDIR]     (default tests/fuzz/corpus)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$here/corpus}
mkdir -p "$OUT/macho" "$OUT/fat" "$OUT/typeenc" "$OUT/codesig" "$OUT/ifunit"
work=$(mktemp -d "${TMPDIR:-/tmp}/hao-seeds.XXXXXX")
trap 'rm -rf "$work"' EXIT
SA=(-Os -Wl,-segalign,0x1000)
clang -arch arm64e "${SA[@]}" -o "$OUT/macho/exec-arm64e" "$here/seeds/exec.c"
clang -arch arm64 "${SA[@]}" -o "$OUT/macho/exec-arm64" "$here/seeds/exec.c"
clang -arch arm64e "${SA[@]}" -dynamiclib -install_name /tmp/libseed.dylib -o "$OUT/macho/lib-arm64e" "$here/seeds/lib.c"
clang -arch arm64e "${SA[@]}" -bundle -o "$OUT/macho/bundle-arm64e" "$here/seeds/lib.c"
# fat: arm64e + x86_64 (lipo aligns the arm64e slice to 16 KiB)
clang -arch x86_64 -Os -o "$work/x86" "$here/seeds/exec.c"
lipo -create "$work/x86" "$OUT/macho/exec-arm64e" -output "$OUT/macho/fat-x86_64-arm64e"
# hao_fat.py inputs: fat and thin files
cp "$OUT/macho/fat-x86_64-arm64e" "$OUT/fat/fat32"
cp "$OUT/macho/lib-arm64e" "$OUT/fat/thin"
python3 - "$OUT/fat" <<'EOF'
import struct, sys
d = sys.argv[1]
# fat64 header with one arm64e slice and one fake x86_64 slice
body = open(d + "/thin", "rb").read()
hdr = struct.pack(">II", 0xCAFEBABF, 2)
hdr += struct.pack(">IIQQII", 0x0100000C, 2, 0x1000, len(body), 12, 0)
hdr += struct.pack(">IIQQII", 0x01000007, 3, 0x1000 + len(body), 64, 12, 0)
blob = hdr + b"\0" * (0x1000 - len(hdr)) + body + b"\xcf\xfa\xed\xfe" + b"\0" * 60
open(d + "/fat64", "wb").write(blob)
EOF
# embedded code signatures: the linker's ad hoc one, codesign's with
# requirements, XML and DER entitlements (sealed special slots), one with
# two CodeDirectories (SHA-1 and SHA-256: the reader takes the higher rank),
# and a hardened-runtime one with disable-library-validation
printf '<?xml version="1.0"?><plist version="1.0"><dict><key>com.apple.security.get-task-allow</key><true/></dict></plist>' > "$work/ents.plist"
cp "$OUT/macho/exec-arm64e" "$work/signed"
codesign -s - -f --entitlements "$work/ents.plist" "$work/signed" 2>/dev/null
cp "$OUT/macho/exec-arm64e" "$work/signed2"
codesign -s - -f --digest-algorithm=sha1,sha256 --entitlements "$work/ents.plist" "$work/signed2" 2>/dev/null
printf '<?xml version="1.0"?><plist version="1.0"><dict><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>' > "$work/dlv.plist"
cp "$OUT/macho/exec-arm64e" "$work/signed3"
codesign -s - -f -o runtime --entitlements "$work/dlv.plist" "$work/signed3" 2>/dev/null
python3 - "$OUT/codesig" "$OUT/macho/exec-arm64e" "$work/signed" "$work/signed2" "$work/signed3" <<'PY'
import struct, sys
out = sys.argv[1]
for name, p in (("linker-adhoc", sys.argv[2]), ("codesign-ents", sys.argv[3]), ("codesign-two-digests", sys.argv[4]),
                ("codesign-runtime-dlv", sys.argv[5])):
    d = open(p, "rb").read()
    ncmds, o = struct.unpack_from("<I", d, 16)[0], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, o)
        if cmd == 0x1D:
            off, n = struct.unpack_from("<II", d, o + 8)
            open(out + "/" + name, "wb").write(d[off:off + n])
        o += size
PY
# type encodings: what clang emits for ObjC/blocks signatures, one per file
cat > "$work/enc.m" <<'EOF'
#import <Foundation/Foundation.h>
#include <stdio.h>
typedef struct { float a, b; } F2;
typedef struct { double m[4][4]; } M4;
typedef struct { char c; short s; long long q; } Mixed;
typedef union { int i; float f; double d; } U;
typedef struct { unsigned a : 3, b : 5; unsigned long long c : 40; } Bits;
typedef struct Node { struct Node *next; int v[3]; } Node;
int main(void)
{
    const char *e[] = {@encode(NSRect), @encode(NSRange), @encode(F2), @encode(M4), @encode(Mixed), @encode(U),
                       @encode(Bits), @encode(Node), @encode(CGAffineTransform), @encode(NSDecimal),
                       "v24@0:8@?<v@?@\"NSString\">16", "{CGRect={CGPoint=dd}{CGSize=dd}}40@0:8@16{_NSRange=QQ}24",
                       "^{__CFString=}16@0:8", "[8^{Node}]", "(U=ifd)", "@\"NSArray\"24@0:8^@16", "Vv16@0:8",
                       "rn^{F2=ff}", "b3b5b40", "D16@0:8"};
    for (unsigned k = 0; k < sizeof e / sizeof *e; k++) printf("%s\n", e[k]);
    return 0;
}
EOF
clang -arch x86_64 -framework Foundation -o "$work/enc" "$work/enc.m"
codesign -s - -f "$work/enc" 2>/dev/null
n=0
timeout -s KILL 20 "$work/enc" | while IFS= read -r line; do
    n=$((n + 1)); printf '%s' "$line" > "$OUT/typeenc/enc$n"
done
# interface units (tools/hao_ifunit.py): every class, kind and field, thunk pairs; and 300 masked records
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    _exact EXACT - 0x3 - - - _typed_b TYPED B - 0x1 - - _typed_swift TYPED t 0x101 - - - \
    _typed_sig TYPED A 0xff 0x2 'i{?=[8I]}' - _refused REFUSED - - - - 'std::string by value' \
    _unknown UNKNOWN - - - - 'no prototype' '_$s4main3fooyyF' EXACT - 0x0 - - - > "$work/all.tsv"
printf '_zthunk\tTYPED\tA\t0x5\t-\tv{?=dd}\t-\tk0123_ab\n_zthunk2\tTYPED\t-\t0x0\t-\t-\t-\tD_9\n' >> "$work/all.tsv"
python3 "$here/../../tools/hao_ifunit.py" write "$OUT/ifunit/all" --uuid 00112233445566778899aabbccddeeff "$work/all.tsv"
for k in $(seq 0 299); do printf '_f%04d\tEXACT\t-\t%#x\t-\t-\t-\n' $k $((k * 7919 % 65536)); done > "$work/many.tsv"
python3 "$here/../../tools/hao_ifunit.py" write "$OUT/ifunit/many" --uuid ffeeddccbbaa99887766554433221100 "$work/many.tsv"
for f in "$OUT"/macho/* "$OUT"/fat/* "$OUT"/typeenc/* "$OUT"/codesig/* "$OUT"/ifunit/*; do printf '%7d %s\n' "$(wc -c < "$f")" "${f#$OUT/}"; done
