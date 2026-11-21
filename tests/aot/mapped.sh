#!/bin/bash
# Exercise normal guest startup with a test-only launcher whose signed image
# pins this harness's one cache. No ordinary hao executable carries this pin.
set -eu
: "${OUT:?}" "${B:?}"
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
cache=$(mktemp -d "$OUT/cache.XXXXXX")
guest=$cache/hello
clang -arch arm64e -O1 "$repo/tests/guest/hello.c" -o "$guest"
codesign -s - -f "$guest"
timeout -s KILL 60 "$B/hao" --aot --cache "$cache" "$guest"
timeout -s KILL 60 "$B/hao" --aot --mapped-input --cache "$cache" "$guest"
python3.14 -I "$repo/tools/hao-aot-mapped" "$cache"/*.unsigned
pin=$(echo "$cache"/*.aot9.sha256)
for invalid in check empty; do
    if [ "$invalid" = check ]; then set -- --check "$guest"; else set --; fi
    rc=0
    timeout -s KILL 30 "$B/hao" --aot --mapped-input --cache "$cache" "$@" >/dev/null 2>&1 || rc=$?
    test "$rc" = 64
done
mkdir "$cache/only"
cp "${pin%.sha256}" "$cache/only/"
rc=0
timeout -s KILL 30 "$B/hao" --aot --check --cache "$cache/only" "$guest" >"$cache/only.log" 2>&1 || rc=$?
test "$rc" = 4
grep -F "mapped trial requires executable admission" "$cache/only.log"
clang -arch x86_64 -O1 -g -fsanitize=address,undefined -I"$repo/include" "$repo/tests/aot/mapped_policy.c" \
    "$repo/src/aot_mapped.c" "$repo/src/aot_policy.c" "$repo/src/cpufeat.c" -L"$B" -lhao \
    -Wl,-rpath,"$B" -o "$cache/policy"
codesign -s - -f "$cache/policy"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout -s KILL 30 "$cache/policy" "${pin%.sha256}" "$cache"/*.aot
pin_launcher() {
    python3.14 -I - "$cache" <<'PYTHON'
from pathlib import Path
import sys
p = Path(sys.argv[1])
pins = sorted(p.glob("*.aot9.sha256"))
(p / "pins").write_bytes(b"".join(bytes.fromhex(f.name[:32]) + f.read_bytes() for f in pins))
PYTHON
    clang -arch x86_64 "$repo/src/front/cli.c" -L"$B" -lhao -lhao_identity \
        -Wl,-sectcreate,__TEXT,__hao_aot_trial,"$cache/pins" -Wl,-rpath,"$B" -o "$cache/runner"
}
pin_launcher
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$cache/runner"
run() {
    local name=$1
    shift
    local rc=0
    timeout -s KILL 30 "$@" >"$cache/$name.out" 2>"$cache/$name.err" || rc=$?
    if [ "$rc" != 7 ]; then cat "$cache/$name.err" >&2; return 1; fi
}
run native "$guest"
if [ "${EXPECT_FIXTURE_REFUSAL:-0}" = 1 ]; then
    run restricted env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$guest"
    cmp "$cache/native.out" "$cache/restricted.out"
    grep -F "each block copied" "$cache/restricted.err"
    grep -F "mapped_lookups=0" "$cache/restricted.err"
    if grep -q "mapped AOT text loaded" "$cache/restricted.err"; then exit 1; fi
    exit 0
fi
run interp "$B/hao" --interp "$guest"
run jit env HAO_NO_AOT=1 "$B/hao" "$guest"
run copy env HAO_CACHE="$cache" HAO_VERBOSE=1 "$B/hao" --stats "$guest"
run mapped env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$guest"
for mode in interp jit copy mapped; do cmp "$cache/native.out" "$cache/$mode.out"; done
grep -F "each block copied" "$cache/copy.err"
grep -F "mapped AOT text loaded" "$cache/mapped.err"
grep -E "aot_lazy_blobs=0 aot_lazy_kb=0 aot_lazy_bursts=0 mapped_full=0 mapped_lookups=[1-9][0-9]* wx_writes=0 " "$cache/mapped.err"
run alias env HAO_CODE_ALIAS=1 HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$guest"
cmp "$cache/native.out" "$cache/alias.out"
grep -F "mapped AOT text loaded" "$cache/alias.err"
run nochain env HAO_NO_CHAIN=1 HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" "$guest"
cmp "$cache/native.out" "$cache/nochain.out"
grep -F "each block copied" "$cache/nochain.err"
run unpinned env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$B/hao" "$guest"
grep -F "each block copied" "$cache/unpinned.err"
# Build a second resident image and enter it both by BL and from an
# indirect callback. Both descriptors must survive and serve the same run.
printf 'int mapped_add(int a, int (*cb)(int)) { return cb(a + 1); }\n' > "$cache/dep.c"
cat > "$cache/main.c" <<'C'
#include <stdio.h>
extern int mapped_add(int, int (*)(int));
static int twice(int a) { return 2 * a; }
int main(void) { printf("dependency %d\n", mapped_add(20, twice)); return 7; }
C
clang -arch arm64e -O1 -dynamiclib "$cache/dep.c" -install_name @loader_path/libmapped.dylib -o "$cache/libmapped.dylib"
codesign -s - -f "$cache/libmapped.dylib"
clang -arch arm64e -O1 "$cache/main.c" "$cache/libmapped.dylib" -o "$cache/two"
codesign -s - -f "$cache/two"
for f in "$cache/libmapped.dylib" "$cache/two"; do
    timeout -s KILL 60 "$B/hao" --aot --mapped-input --cache "$cache" "$f"
done
for f in "$cache"/*.unsigned; do python3.14 -I "$repo/tools/hao-aot-mapped" "$f"; done
pin_launcher
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$cache/runner"
run native2 "$cache/two"
run mapped2 env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$cache/two"
cmp "$cache/native2.out" "$cache/mapped2.out"
test "$(grep -c 'mapped AOT text loaded' "$cache/mapped2.err")" = 2
# Only the main and libmapped have approved artifacts. An uncached dylib
# fills a 1 MiB JIT arena; its caller then first enters mapped_add.
cat > "$cache/fill.s" <<'ASM'
.text
.p2align 2
.globl _fill
_fill:
.rept 30000
add x0, x0, #1
b 1f
1:
.endr
ret
ASM
clang -arch arm64e -dynamiclib "$cache/fill.s" -install_name @loader_path/libfill.dylib -o "$cache/libfill.dylib"
codesign -s - -f "$cache/libfill.dylib"
cat > "$cache/fork.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
extern int mapped_add(int, int (*)(int));
extern long fill(long);
static int twice(int a) { return 2 * a; }
int main(void) {
    if (fill(1) != 30001) return 2;
    pid_t child = fork();
    if (child < 0) return 3;
    if (!child) _exit(mapped_add(20, twice) == 42 ? 0 : 4);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 5;
    printf("post-fork %d\n", mapped_add(15, twice));
    return 7;
}
C
clang -arch arm64e -O1 "$cache/fork.c" "$cache/libfill.dylib" "$cache/libmapped.dylib" -o "$cache/fork"
codesign -s - -f "$cache/fork"
timeout -s KILL 60 "$B/hao" --aot --mapped-input --cache "$cache" "$cache/fork"
for f in "$cache"/*.unsigned; do python3.14 -I "$repo/tools/hao-aot-mapped" "$f"; done
pin_launcher
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$cache/runner"
run fork-native "$cache/fork"
run fork-mapped env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 HAO_JIT_CACHE_MB=1 \
    "$cache/runner" --stats "$cache/fork"
cmp "$cache/fork-native.out" "$cache/fork-mapped.out"
grep -F "code cache full" "$cache/fork-mapped.err"
grep -E "mapped_full=[1-9]" "$cache/fork-mapped.err"
# This native differential has a large translated block which faults at a
# load with live NZCV. Its guest handler verifies the exact PC and flags.
clang -arch arm64e -march=armv8.5-a+fp16 -O1 -fno-builtin "$repo/tests/guest/fma16.c" -o "$cache/recovery"
codesign -s - -f "$cache/recovery"
timeout -s KILL 60 "$B/hao" --aot --mapped-input --cache "$cache" "$cache/recovery"
for f in "$cache"/*.unsigned; do python3.14 -I "$repo/tools/hao-aot-mapped" "$f"; done
pin_launcher
codesign -s - -f --entitlements "$repo/tools/hao.entitlements" "$cache/runner"
timeout -s KILL 60 "$cache/recovery" > "$cache/recovery.native"
env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 timeout -s KILL 60 "$cache/runner" --stats "$cache/recovery" \
    > "$cache/recovery.mapped" 2> "$cache/recovery.err"
cmp "$cache/recovery.native" "$cache/recovery.mapped"
grep -F "large-block PC/NZCV recovery PASS" "$cache/recovery.mapped"
grep -F "mapped AOT text loaded" "$cache/recovery.err"
grep -E "mapped_locates=[1-9]" "$cache/recovery.err"
# Re-sign a modified container. A valid replacement signature cannot
# authorize the change against the digest in the signed launcher.
python3.14 -I - "${pin%.sha256}" "$repo/tests/aot/patch.py" <<'PYTHON'
from pathlib import Path
import runpy, struct, sys
p = Path(sys.argv[1])
v8 = runpy.run_path(sys.argv[2])
blob = bytearray(p.read_bytes())
start, length, text, _ = struct.unpack_from("<4Q", blob, 512 + 40)
assert blob[text] != 0xcc
blob[text] = 0xcc
hdr = v8["HDR"]
fields = list(hdr.unpack_from(blob, start))
fields[11] = 0
fields[11] = v8["hash64"](blob[start + hdr.size:start + length], v8["hash64"](hdr.pack(*fields)))
hdr.pack_into(blob, start, *fields)
p.write_bytes(blob)
PYTHON
codesign -f -s - -i org.hao.changed "${pin%.sha256}"
codesign --verify --strict "${pin%.sha256}"
run corrupt env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" "$guest"
cmp "$cache/native.out" "$cache/corrupt.out"
grep -F "unapproved mapped file digest" "$cache/corrupt.err"
grep -F "each block copied" "$cache/corrupt.err"
