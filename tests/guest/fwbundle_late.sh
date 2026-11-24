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
__attribute__((used,section("__DATA,__hao_named"))) static const int observed_image = 17;
@interface ProbeGuestFrameworkClass : NSObject @end
@implementation ProbeGuestFrameworkClass @end
static int initialized;
__attribute__((constructor)) static void inspect_bundle(void) {
    if (getenv("LINKED_FRAMEWORK")) {
        initialized = 1;
        return;
    }
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
# No identifier lookup in the linked constructor may seed Foundation's enumeration.
cat > "$out/linked.m" <<'SOURCE'
#import <Foundation/Foundation.h>
extern int fixture_value(void);
int main(void) {
    @autoreleasepool {
        if (fixture_value() != 74) return 1;
        unsigned matches = 0;
        for (NSBundle *bundle in NSBundle.allFrameworks)
            if ([bundle.bundleIdentifier isEqual:@"org.hao.fixture.lifecycle"]) matches++;
        printf("linked count=%u\n", matches);
        return matches != 1;
    }
}
SOURCE
xcrun clang -arch arm64 -fobjc-arc -framework Foundation "$out/linked.m" -F "$out" -framework Foo -Wl,-rpath,"$out" -o "$out/linked"
LINKED_FRAMEWORK=1 "$out/linked" > "$out/native"
LINKED_FRAMEWORK=1 timeout -s KILL 60 "$HAO" "$out/linked" > "$out/guest"
diff -u "$out/native" "$out/guest"
# A native caller and a guest caller share Foundation's one-time enumeration.
# Query the underlying Objective-C list separately after the load in both cases.
cat > "$out/first.m" <<'SOURCE'
#import <Foundation/Foundation.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>
#import <objc/runtime.h>
static int callbacks;
static void observe(const struct mach_header *header) {
    unsigned long size;
    if (!getsectiondata((const void *)header, "__DATA", "__hao_named", &size)) return;
    unsigned matches = 0;
    for (NSBundle *bundle in NSBundle.allFrameworks)
        if ([bundle.bundleIdentifier isEqual:@"org.hao.fixture.lifecycle"]) matches++;
    if (matches != 1 || !objc_getClass("ProbeGuestFrameworkClass")) abort();
    callbacks++;
}
void watch_names(void) { objc_addLoadImageFunc(observe); }
int watched_names(void) { return callbacks; }
void native_first(void) { (void)NSBundle.allFrameworks; }
SOURCE
for arch in arm64 x86_64; do
    xcrun clang -arch "$arch" -fobjc-arc -framework Foundation -dynamiclib "$out/first.m" -install_name "$out/first.dylib" -o "$out/first-$arch.dylib"
done
cp "$out/first-arm64.dylib" "$out/first.dylib"
cat > "$out/mixed.m" <<'SOURCE'
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <sys/stat.h>
extern void native_first(void), watch_names(void);
extern int watched_names(void);
static int listed(const char **names, unsigned count, const struct stat *target) {
    unsigned found = 0;
    for (unsigned k = 0; names[k]; k++) {
        struct stat st;
        if (stat(names[k], &st) == 0 && st.st_dev == target->st_dev && st.st_ino == target->st_ino) found++;
        if (k >= count) return -1;
    }
    return found;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) return 1;
        int watching = !strcmp(argv[1], "observer");
        if (watching) watch_names();
        if (!strcmp(argv[1], "native-first")) native_first();
        if (!strcmp(argv[1], "guest-first")) (void)NSBundle.allFrameworks;
        if (!dlopen(argv[2], RTLD_NOW)) return 2;
        struct stat file;
        if (stat(argv[2], &file)) return 3;
        unsigned count = 0;
        const char **names = objc_copyImageNames(&count);
        int found = listed(names, count, &file);
        free(names);
        names = objc_copyImageNames(NULL);
        int uncounted = listed(names, UINT_MAX, &file);
        free(names);
        int frameworks = 0;
        for (NSBundle *bundle in NSBundle.allFrameworks)
            if ([bundle.bundleIdentifier isEqual:@"org.hao.fixture.lifecycle"]) frameworks++;
        printf("names=%d uncounted=%d frameworks=%d\n", found, uncounted, frameworks);
        return found != 1 || uncounted != 1 || frameworks != (watching || !strcmp(argv[1], "late-first")) ||
               (watching && watched_names() != 1);
    }
}
SOURCE
xcrun clang -arch arm64 -fobjc-arc -framework Foundation "$out/mixed.m" "$out/first.dylib" -Wl,-rpath,"$out" -o "$out/mixed"
export LINKED_FRAMEWORK=1
for mode in native-first guest-first late-first; do "$out/mixed" "$mode" "$fw/Foo" >> "$out/native-mixed"; done
cp "$out/first-x86_64.dylib" "$out/first.dylib"
for mode in native-first guest-first late-first; do timeout -s KILL 60 "$HAO" "$out/mixed" "$mode" "$fw/Foo" >> "$out/guest-mixed"; done
diff -u "$out/native-mixed" "$out/guest-mixed"
# libobjc documents that native load-image callbacks run under its internal
# lock. Hao guest callbacks instead have the loader lock and can enumerate.
timeout -s KILL 10 "$HAO" "$out/mixed" observer "$fw/Foo" > "$out/observer-mixed"
grep -qx 'names=1 uncounted=1 frameworks=1' "$out/observer-mixed"
