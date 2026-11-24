#!/bin/sh
# Plain authored CLI/framework only. No service or GUI activation.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
runtime=${B:-"$root/build"}
runtime=$(cd "$runtime" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cd "$dir"
framework="$dir/Plain.framework"
mkdir -p "$framework/Versions/A/Resources"
ln -s A "$framework/Versions/Current"
for member in Resources Plain; do ln -s "Versions/Current/$member" "$framework/$member"; done
cat > "$framework/Resources/Info.plist" <<'EOF'
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.hao.test.plain</string><key>CFBundleExecutable</key><string>Plain</string><key>CFBundlePackageType</key><string>FMWK</string><key>CFBundleVersion</key><string>1</string></dict></plist>
EOF
echo 'int plain(void) { return 0; }' > plain.c
xcrun clang -arch arm64 -dynamiclib -install_name @rpath/Plain.framework/Versions/A/Plain plain.c -o "$framework/Versions/A/Plain"
codesign -f -s - "$framework"
cat > args.c <<'EOF'
#include <stdio.h>
int main(int argc, char **argv) { if (argc != 2) return 3; puts(argv[1]); return 0; }
EOF
xcrun clang -arch arm64 args.c -o ArgProbe
codesign -f -s - --identifier org.hao.test.argprobe ArgProbe
"$root/tools/hao-prepare-framework" --separate-adapter --associate-original "$framework/Plain" "$framework" "$dir/OwnAdapter.framework"
"$root/tools/hao-companion-launcher" "$dir/ArgProbe" "$dir/OwnAdapter.framework" --runtime "$runtime" -o "$dir/Owner"
DYLD_LIBRARY_PATH="$runtime" "$dir/Owner" --aot > before.txt
grep -Fqx -- '--aot' before.txt
echo 'int main(void) {return 0;}' > replacement.c
xcrun clang -arch arm64 replacement.c -o new-program
codesign -f -s - --identifier org.hao.test.argprobe new-program
mv new-program ArgProbe
if DYLD_LIBRARY_PATH="$runtime" "$dir/Owner" --aot > after.txt 2>&1; then
    echo 'replacement unexpectedly executed' >&2
    exit 1
fi
grep -Ei 'cdhash|bound|CodeDirectory|slice record|binding' after.txt
