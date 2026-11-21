#!/bin/sh
# Lazy AOT install copies blocks in bursts (jit.c aot_lazy_fill): a guest that calls N small functions once each runs
# from its AOT cache, prints what native arm64 prints, and its blocks come in through far fewer write aliases than
# blocks (each alias is two mach_vm_map calls: hao_wx_begin, hao_wx_end). --stats' hao-jit line gives aot_blocks
# (blocks copied in), aot_lazy_bursts and wx_writes (every write to executable memory, chaining included); the
# test needs aot_lazy_bursts >= 1 and wx_writes * RATIO <= aot_blocks. One block per alias, as before bursts,
# gives wx_writes >= aot_blocks. The chain fixtures prime source return arms
# and distant targets before taking cut branches, serially and on four threads.
#   OUT=dir HAO=/abs/build/hao [T=120] [N=2000] [RATIO=8] tests/guest/aotburst.sh
set -eu
OUT=${OUT:-$(mktemp -d -t hao-aotburst)}
HAO=${HAO:?HAO=build/hao}
T=${T:-120}
N=${N:-2000}
RATIO=${RATIO:-8}
ROOT=$OUT
for mode in copy chain threads; do
OUT=$ROOT/$mode
mkdir -p "$OUT"
rm -rf "$OUT/c"; mkdir -p "$OUT/c"; chmod 700 "$OUT/c"
python3 - "$OUT/burst.c" "$N" "$mode" <<'EOF'
import sys
out, n, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
if mode in ("chain", "threads"):
    # Prime every source via its return arm, then every distant target, before
    # taking any cut branch. This forces many ready links per source burst.
    with open(out.replace(".c", ".S"), "w") as asm:
        asm.write(".text\n")
        for k in range(n):
            asm.write(f".p2align 2\n.globl _f{k}\n_f{k}:\ncbz w0, 1f\nb _t{k}\n1: ret\n")
        for k in range(n):
            asm.write(f".p2align 2\n.globl _t{k}\n_t{k}:\nadd w0, w0, #{k % 4096}\nret\n")
    with open(out, "w") as f:
        f.write("#include <stdio.h>\n#include <pthread.h>\n#include <stdint.h>\n")
        for k in range(n):
            f.write(f"unsigned f{k}(unsigned), t{k}(unsigned);\n")
        for name in ("f", "t"):
            f.write("unsigned (*const %sns[%d])(unsigned) = {%s};\n" %
                    (name, n, ", ".join(f"{name}{k}" for k in range(n))))
        if mode == "threads":
            f.write("static void *run(void *arg) { uintptr_t s=0; "
                    "for (int k=0;k<%d;k++) s += fns[k](1); return (void *)s; }\n" % n)
        f.write("int main(void) { unsigned s = 0; "
                "for (int k=0;k<%d;k++) s += fns[k](0); "
                "for (int k=0;k<%d;k++) s += tns[k](1); " % (n,n))
        if mode == "threads":
            f.write("pthread_t threads[4]; for (int j=0;j<4;j++) "
                    "if (pthread_create(&threads[j],0,run,0)) return 2; "
                    "for (int j=0;j<4;j++) { void *v; "
                    "if (pthread_join(threads[j],&v)) return 3; s += (uintptr_t)v; } ")
        else:
            f.write("for (int k=0;k<%d;k++) s += fns[k](1); " % n)
        f.write('printf("sum=%u\\n",s); return 0; }\n')
    sys.exit(0)
open(out.replace(".c", ".S"), "w").write(".text\n")
with open(out, "w") as f:
    f.write("#include <stdio.h>\n")
    for k in range(n):
        f.write("__attribute__((noinline)) unsigned f%d(unsigned x) { return (x * %du) ^ (x >> %d); }\n" % (k, 2 * k + 3, k % 13 + 1))
    f.write("unsigned (*const fns[%d])(unsigned) = {%s};\n" % (n, ", ".join("f%d" % k for k in range(n))))
    f.write("int main(void) { unsigned s = 1; for (int k = 0; k < %d; k++) s = s * 31 + (unsigned)fns[k](s); "
            "printf(\"sum=%%u\\n\", s); return 0; }\n" % n)
EOF
for a in arm64 arm64e; do
    clang -arch $a -O1 -o "$OUT/burst.$a" "$OUT/burst.c" "$OUT/burst.S" || { echo "aotburst: build $a failed"; exit 2; }
    codesign -s - -f "$OUT/burst.$a" 2>/dev/null
done
want=$(timeout -s KILL "$T" "$OUT/burst.arm64")
timeout -s KILL "$T" "$HAO" --aot --cache "$OUT/c" "$OUT/burst.arm64e" > "$OUT/aot.log" 2>&1 ||
    { echo "aotburst: hao --aot failed: $(tail -2 "$OUT/aot.log")"; exit 2; }
got=$(HAO_CACHE="$OUT/c" timeout -s KILL "$T" "$HAO" --stats "$OUT/burst.arm64e" 2> "$OUT/run.err")
line=$(grep '^hao-jit: aot_blocks=' "$OUT/run.err" | tail -1)
num() { printf '%s\n' "$line" | sed -n "s/.* $1=\([0-9]*\).*/\1/p; s/^hao-jit: $1=\([0-9]*\).*/\1/p" | head -1; }
blocks=$(num aot_blocks); blobs=$(num aot_lazy_blobs); bursts=$(num aot_lazy_bursts); wx=$(num wx_writes)
echo "aot_blocks=$blocks aot_lazy_blobs=$blobs aot_lazy_bursts=$bursts wx_writes=$wx (mode=$mode N=$N)"
if [ "$got" != "$want" ]; then echo "aotburst: FAIL output '$got', native '$want'"; exit 1; fi
if [ -z "$blocks" ] || [ -z "$wx" ] || [ "${blobs:-0}" -lt 1 ]; then echo "aotburst: FAIL the cache was not installed lazily: $line"; exit 1; fi
if [ "${bursts:-0}" -lt 1 ] || [ $((wx * RATIO)) -gt "$blocks" ]; then
    echo "aotburst: FAIL $wx write aliases for $blocks blocks (want at most 1 per $RATIO)"; exit 1
fi
echo "aotburst: PASS ($blocks blocks in $bursts bursts, $wx write aliases)"

done
