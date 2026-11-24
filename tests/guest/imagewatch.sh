#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
src="$root/tests/guest/imagewatch.m"
out=${OUT:-"$root/build/imagewatch"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
clang -arch arm64 -arch x86_64 -DWATCHER -dynamiclib "$src" -framework Foundation -install_name "$out/watcher.dylib" -o "$out/watcher.dylib"
clang -arch arm64 -DIMAGE_KIND=1 -dynamiclib "$src" -framework Foundation -o "$out/late.dylib"
clang -arch arm64 -DIMAGE_KIND=2 -dynamiclib "$src" -framework Foundation -o "$out/nested.dylib"
clang -arch arm64 -DIMAGE_KIND=0 "$src" "$out/watcher.dylib" -framework Foundation -o "$out/main"
for f in watcher.dylib late.dylib nested.dylib main; do codesign -f -s - "$out/$f" 2>/dev/null; done
"$out/main" "$out/late.dylib" > "$out/native.out"
printf 'image 0\nimage 1\nclass load\nreplay 0\nreplay 1\n' > "$out/expected"
diff -u "$out/expected" "$out/native.out"
HAO_NATIVE_APP_LIBS=1 "${HAO:-$root/build/hao}" "$out/main" "$out/late.dylib" > "$out/translated.out"
diff -u "$out/native.out" "$out/translated.out"
# Native libobjc forbids a recursive dlopen from its load-image callback.
# The guest loader permits it and delivers older headers before the nested one.
HAO_NATIVE_APP_LIBS=1 "${HAO:-$root/build/hao}" "$out/main" "$out/late.dylib" "$out/nested.dylib" > "$out/nested.out"
printf 'nested 0\nnested 1\nnested 2\n' >> "$out/expected"
diff -u "$out/expected" "$out/nested.out"
