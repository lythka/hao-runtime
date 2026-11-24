#!/bin/sh
# Compare late dlopen discovery against a native client with the same framework.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
HAO=${HAO:-$here/../../build/hao}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
fw="$out/Foo.framework"
mkdir -p "$fw/Versions/A/Resources"
ln -s A "$fw/Versions/Current"
ln -s Versions/Current/Foo "$fw/Foo"
ln -s Versions/Current/Resources "$fw/Resources"
cat > "$fw/Resources/Info.plist" <<'PLIST'
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.hao.fixture.lifecycle</string><key>CFBundleExecutable</key><string>Foo</string><key>CFBundlePackageType</key><string>FMWK</string></dict></plist>
PLIST
cat > "$out/fixture.m" <<'SOURCE'
#import <Foundation/Foundation.h>
static int initialized;
__attribute__((constructor)) static void inspect_bundle(void) {
    NSBundle *bundle = [NSBundle bundleWithIdentifier:@"org.hao.fixture.lifecycle"];
    initialized = bundle && bundle.executablePath;
    printf("constructor found=%d loaded=%d\n", !!bundle, bundle.loaded);
}
int fixture_value(void) { return initialized ? 74 : 0; }
SOURCE
xcrun clang -arch arm64 -fobjc-arc -framework Foundation -dynamiclib "$out/fixture.m" -install_name @rpath/Foo.framework/Versions/A/Foo -o "$fw/Versions/A/Foo"
codesign -fs - "$fw"
xcrun clang -arch arm64 -fobjc-arc -framework Foundation "$here/fwbundle_late.m" -o "$out/observer"
ln -s "$out" "$out/alias"
for mode in fresh precreated aliased; do
    unset ALIAS_BUNDLE
    path=$fw
    if [ "$mode" = aliased ]; then path="$out/alias/Foo.framework"; export ALIAS_BUNDLE=1; fi
    if [ "$mode" = precreated ]; then export PRECREATE=1; else unset PRECREATE; fi
    "$out/observer" "$path" > "$out/native"
    timeout -s KILL 60 "$HAO" "$out/observer" "$path" > "$out/guest"
    diff -u "$out/native" "$out/guest"
    grep -q '^constructor found=1' "$out/native"
    grep -Eq '^after .*idFound=1' "$out/native"
done
