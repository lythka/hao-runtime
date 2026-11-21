#!/bin/bash
# OUT is a caller-owned scratch directory. Exit 2 means host policy denied
# the positive fixture. A production runtime never consults this probe.
set -eu
: "${OUT:?}" "${B:?}"
mkdir -p "$OUT"
repo=$(cd "$(dirname "$0")/../.." && pwd)
printf 'int hao_fixture(void) { return 7; }\n' > "$OUT/fixture.c"
clang -arch x86_64 -bundle "$OUT/fixture.c" -o "$OUT/fixture.bundle"
codesign -s - -f "$OUT/fixture.bundle"
clang -arch x86_64 -I"$repo/include" "$repo/tests/aot/mapped_admission.c" -L"$B" -lhao \
    -Wl,-rpath,"$B" -o "$OUT/probe"
codesign -s - -f "$OUT/probe"
digest=$(shasum -a 256 "$OUT/fixture.bundle" | cut -d' ' -f1)
timeout -s KILL 30 "$OUT/probe" "$OUT/fixture.bundle" "$digest"
cp "$OUT/fixture.bundle" "$OUT/changed.bundle"
printf x >> "$OUT/changed.bundle"
if timeout -s KILL 30 "$OUT/probe" "$OUT/changed.bundle" "$digest"; then exit 1; else rc=$?; fi
test "$rc" = 1
