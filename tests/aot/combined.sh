#!/bin/bash
# Mapped caller -> batched copy -> mapped callback, compared with native ARM.
set -eu
: "${OUT:?}" "${B:?}"
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
cache=$(mktemp -d "$OUT/cache.XXXXXX")
chmod 700 "$cache"
python3.14 - "$cache" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
n = 2000
with (p/'dep.S').open('w') as f:
    f.write('.text\n')
    for k in range(n):
        f.write(f'.p2align 2\n.globl _f{k}\n_f{k}:\ncbz w0, 1f\nb _t{k}\n1: ret\n')
    for k in range(n):
        f.write(f'.p2align 2\n.globl _t{k}\n_t{k}:\nadd w0, w0, #{k}\nret\n')
with (p/'dep.c').open('w') as f:
    for k in range(n):
        f.write(f'unsigned f{k}(unsigned), t{k}(unsigned);\n')
    for name in ('f', 't'):
        f.write('unsigned (*const %sns[])(unsigned) = {%s};\n' %
                (name, ','.join(f'{name}{k}' for k in range(n))))
    f.write('unsigned copied(unsigned (*cb)(unsigned)) { unsigned s=0;\n')
    for name, arg in [('f',0),('t',1),('f',1)]:
        f.write(f'for(int i=0;i<{n};i++) s += {name}ns[i]({arg});\n')
    f.write('return cb(s); }\n')
(p/'main.c').write_text('''#include <stdio.h>
extern unsigned copied(unsigned (*)(unsigned));
static unsigned callback(unsigned x) { return x * 3 + 7; }
int main(void) { printf("combined %u\\n", copied(callback)); return 0; }
''')
PY
for arch in arm64 arm64e; do
    clang -arch "$arch" -O1 -dynamiclib "$cache/dep.c" "$cache/dep.S" \
        -install_name "@loader_path/libdep.$arch.dylib" -o "$cache/libdep.$arch.dylib"
    codesign -f -s - "$cache/libdep.$arch.dylib"
    clang -arch "$arch" -O1 "$cache/main.c" "$cache/libdep.$arch.dylib" -o "$cache/main.$arch"
    codesign -f -s - "$cache/main.$arch"
done
timeout -s KILL 60 "$cache/main.arm64" > "$cache/native.out"
for guest in "$cache/main.arm64e" "$cache/libdep.arm64e.dylib"; do
    timeout -s KILL 120 "$B/hao" --aot --cache "$cache" "$guest"
done
timeout -s KILL 120 "$B/hao" --aot --mapped-input --cache "$cache" "$cache/main.arm64e"
python3.14 -I "$repo/tools/hao-aot-mapped" "$cache"/*.unsigned
python3.14 -I - "$cache" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
f,=p.glob('*.aot9.sha256')
(p/'pins').write_bytes(bytes.fromhex(f.name[:32])+f.read_bytes())
PY
clang -arch x86_64 "$repo/src/front/cli.c" -L"$B" -lhao -lhao_identity \
    -Wl,-sectcreate,__TEXT,__hao_aot_trial,"$cache/pins" -Wl,-rpath,"$B" -o "$cache/runner"
codesign -f -s - --entitlements "$repo/tools/hao.entitlements" "$cache/runner"
run() {
    local name=$1
    shift
    timeout -s KILL 120 "$@" > "$cache/$name.out" 2> "$cache/$name.err"
    cmp "$cache/native.out" "$cache/$name.out"
}
check_copy() {
    python3.14 - "$cache/$1.err" <<'PY'
import re,sys
s=open(sys.argv[1]).read()
line=next(x for x in s.splitlines() if x.startswith('hao-jit: aot_blocks='))
v={k:int(n) for k,n in re.findall(r'(\w+)=(\d+)',line)}
assert v['aot_lazy_blobs'] >= 1 and v['aot_lazy_bursts'] > 0, line
assert v['jit_blocks'] == 0, line
assert v['aot_blocks'] >= 8000 and v['wx_writes']*8 <= v['aot_blocks'], line
print(line)
PY
}
# wx_find refuses aliases outside registered arenas; mapped text is never
# registered. Successful mixed execution therefore also checks that batching
# never tries to alias it. mapped_policy separately checks maximum RX
# protections. The hashes below check backing-file bytes, not private mappings.
shasum -a 256 "$cache"/*.aot9 > "$cache/before.sha"
run mixed env HAO_CACHE="$cache" HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$cache/main.arm64e"
test "$(grep -c 'mapped AOT text loaded' "$cache/mixed.err")" = 1
grep -E 'aot_lazy_blobs=1 .*mapped_lookups=[1-9]' "$cache/mixed.err"
check_copy mixed
# Alias mode must retain the same cross-backend dispatch and copy batching.
run alias env HAO_CACHE="$cache" HAO_CODE_ALIAS=1 HAO_VERBOSE=1 HAO_AOT_MAPPED=1 "$cache/runner" --stats "$cache/main.arm64e"
test "$(grep -c 'mapped AOT text loaded' "$cache/alias.err")" = 1
grep -E 'aot_lazy_blobs=1 .*mapped_lookups=[1-9]' "$cache/alias.err"
check_copy alias
shasum -a 256 "$cache"/*.aot9 > "$cache/after.sha"
cmp "$cache/before.sha" "$cache/after.sha"
run disabled env HAO_CACHE="$cache" "$cache/runner" --stats "$cache/main.arm64e"
check_copy disabled
grep -F 'mapped_lookups=0' "$cache/disabled.err"
# An ordinary launcher has no pin: reject v9 and independently load v8.
run rejected env HAO_CACHE="$cache" HAO_AOT_MAPPED=1 "$B/hao" --stats "$cache/main.arm64e"
check_copy rejected
grep -F 'mapped_lookups=0' "$cache/rejected.err"
mkdir "$cache/empty"
run missing env HAO_CACHE="$cache/empty" HAO_AOT_MAPPED=1 "$cache/runner" --stats "$cache/main.arm64e"
grep -E 'aot_blocks=0 .*jit_blocks=[1-9]' "$cache/missing.err"
run interp "$B/hao" --interp "$cache/main.arm64e"
echo 'combined mapped/copy/native/fallback PASS'
