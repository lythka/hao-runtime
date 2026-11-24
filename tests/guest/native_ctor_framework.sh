#!/bin/bash
# B's native initializer uses a signed guest framework that strongly links B.
# Both trees are relocatable so the identical signed fixture can run on Intel.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
hao=${HAO:-$root/build/hao}
out=${OUT:-$(mktemp -d)}
if [ -z "${OUT:-}" ]; then trap 'rm -rf "$out"' EXIT; fi
mkdir -p "$out"
out=$(cd "$out" && pwd)
cat > "$out/Shape.h" <<'SOURCE'
#import <Foundation/Foundation.h>
typedef struct { double first; long middle; double last; } Shape;
@protocol ShapeReader
+ (Shape)shape;
@end
SOURCE
cat > "$out/A.m" <<'SOURCE'
#import "Shape.h"
#import <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
static char replay_order[4];
static int order_count;
static void post_startup(const struct mach_header *header) {
    unsigned long size;
    char tag = getsectiondata((const void *)header, "__DATA", "__hao_ctor", &size) ? 'G' :
               getsectiondata((const void *)header, "__DATA", "__hao_nested", &size) ? 'H' : 0;
    if (tag && order_count < 3) replay_order[order_count++] = tag;
}
int post_startup_order(void) {
    if (!getenv("CTOR_ORDER")) return 1;
    objc_addLoadImageFunc(post_startup);
    fprintf(stderr, "post-startup replay %s\n", replay_order);
    return !strcmp(replay_order, getenv("CTOR_ORDER"));
}
static int earlier_count;
int earlier_observations(void) { return earlier_count; }
static void earlier(const struct mach_header *header) {
    unsigned long size;
    if (getsectiondata((const void *)header, "__DATA", "__hao_ctor", &size)) earlier_count++;
}
#include <sched.h>
@interface CtorUnrealizedBase : NSObject @end
@implementation CtorUnrealizedBase @end
static pthread_t poller;
static _Atomic int stop;
static int result;
static void *poll_class(void *unused) {
    while (!atomic_load(&stop)) {
        Class<ShapeReader> cls = objc_getClass("CtorVisibilityClass");
        if (!cls) { sched_yield(); continue; }
        Shape shape = [cls shape];
        result = shape.first == 1.5 && shape.middle == 71 && shape.last == 2.25;
        break;
    }
    return NULL;
}
int finish_poll(int available) {
    if (!available) atomic_store(&stop, 1);
    if (pthread_join(poller, NULL)) abort();
    return result;
}
__attribute__((constructor)) static void start(void) {
    puts("A");
    objc_addLoadImageFunc(earlier);
    if (pthread_create(&poller, NULL, poll_class, NULL)) abort();
}
SOURCE
cat > "$out/B.m" <<'SOURCE'
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#ifdef OWN_CLASS
@interface CtorNativeDependencyClass : NSObject @end
@implementation CtorNativeDependencyClass @end
#endif
extern int finish_poll(int);
extern int earlier_observations(void);
__attribute__((used)) static const char signed_marker[] = "ctor-verification-marker";
static int replays;
static void replay(const struct mach_header *header) {
    unsigned long size;
    if (!getsectiondata((const void *)header, "__DATA", "__hao_ctor", &size)) return;
    mach_vm_address_t address = (uintptr_t)getsectiondata((const void *)header, "__DATA_CONST", "__hao_const", &size);
    mach_vm_address_t start = address;
    mach_vm_size_t length = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    if (!address || mach_vm_region(mach_task_self(), &start, &length, VM_REGION_BASIC_INFO_64,
                                   (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
        start > address || info.protection & VM_PROT_WRITE) abort();
    if (object) mach_port_deallocate(mach_task_self(), object);
    replays++;
}
static int answer(Class cls) {
    return cls ? ((int (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("answer")) : -1;
}
static void *worker(void *unused) {
    printf("worker %d\n", answer(objc_getClass("CtorVisibilityClass")));
    return NULL;
}
__attribute__((constructor)) static void start(void) {
    unsigned found = 0;
    for (NSBundle *bundle in NSBundle.allFrameworks)
        if ([bundle.bundleIdentifier isEqual:@"org.hao.fixture.constructor-visibility"]) found++;
    Class cls = objc_getClass("CtorVisibilityClass");
    printf("B framework=%u class=%d value=%d\n", found, cls != Nil, answer(cls));
    printf("racing typed return %d\n", finish_poll(cls != Nil));
    fflush(stdout);
    if (!cls) return;
    objc_addLoadImageFunc(replay);
    printf("replay %d\n", replays);
    const char *nested = getenv("CTOR_NESTED");
    if (!nested || !dlopen(nested, RTLD_NOW)) abort();
    if (getenv("CTOR_G_STARTED")) abort();
    if (getenv("CTOR_CHECK_DEFERRED") && earlier_observations() != 0) abort();
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, NULL)) abort();
}
SOURCE
cat > "$out/G.m" <<'SOURCE'
#import <Foundation/Foundation.h>
__attribute__((used,section("__DATA,__hao_ctor"))) static const int marker = 72;
__attribute__((used,section("__DATA_CONST,__hao_const"))) static const int seal_marker = 73;
#import "Shape.h"
static _Thread_local int value = 71;
#ifdef EXTERNAL_SUPER
@interface CtorUnrealizedBase : NSObject @end
#define SUPER CtorUnrealizedBase
#else
#define SUPER NSObject
#endif
@interface CtorVisibilityClass : SUPER <ShapeReader>
+ (int)answer;
@end
@implementation CtorVisibilityClass
+ (void)load { puts("G class load"); }
+ (int)answer { return ++value; }
+ (Shape)shape { return (Shape){1.5, value, 2.25}; }
@end
#ifdef NATIVE_CLASS_REF
@interface CtorNativeDependencyClass : NSObject @end
__attribute__((constructor)) static void check_native_ref(void) {
    if (![CtorNativeDependencyClass class]) abort();
    puts("native B class ready");
}
#endif
#ifdef MISSING_WEAK_SUPER
@interface CtorAbsentBase : NSObject @end
@interface CtorIgnoredClass : CtorAbsentBase @end
@implementation CtorIgnoredClass @end
__attribute__((constructor)) static void check_ignored(void) {
    extern void *volatile fixture_ignored_ref;
    if (fixture_ignored_ref || [CtorIgnoredClass class]) abort();
    puts("ignored reference nil");
}
#endif
__attribute__((constructor)) static void start(void) { setenv("CTOR_G_STARTED", "1", 1); puts("G"); }
SOURCE
echo 'extern int earlier_observations(void),post_startup_order(void); int main(void) { return earlier_observations() != 1 || !post_startup_order(); }' > "$out/main.c"
echo 'int missing_interface(void) { return 0; }' > "$out/missing.c"
for leg in native guest; do
    dir="$out/$leg"
    fw="$dir/G.framework"
    mkdir -p "$fw/Versions/A/Resources"
    ln -sfn A "$fw/Versions/Current"
    ln -sfn Versions/Current/G "$fw/G"
    ln -sfn Versions/Current/Resources "$fw/Resources"
    cat > "$fw/Resources/Info.plist" <<'PLIST'
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.hao.fixture.constructor-visibility</string><key>CFBundleExecutable</key><string>G</string><key>CFBundlePackageType</key><string>FMWK</string></dict></plist>
PLIST
    native_arch=(-arch arm64 -arch x86_64)
    guest_arch=(-arch arm64 -arch x86_64)
    if [ "$leg" = guest ]; then native_arch=(-arch x86_64); guest_arch=(-arch arm64); fi
    clang "${native_arch[@]}" -fobjc-arc -framework Foundation -dynamiclib "$out/A.m" -install_name @rpath/A.dylib -o "$dir/A.dylib"
    clang "${native_arch[@]}" -fobjc-arc -framework Foundation -dynamiclib "$out/B.m" "$dir/A.dylib" -install_name @rpath/B.dylib -o "$dir/B.dylib"
    echo '#import <Foundation/Foundation.h>
@interface CtorNestedClass : NSObject @end
@implementation CtorNestedClass @end
__attribute__((used,section("__DATA,__hao_nested"))) static const int marker = 1;
__attribute__((constructor)) static void start(void) { puts("H"); }' > "$out/H.m"
    clang "${guest_arch[@]}" -dynamiclib "$out/H.m" -framework Foundation -install_name @rpath/H.dylib -o "$dir/H.dylib"
    codesign -fs - "$dir/H.dylib"
    codesign -fs - "$dir/A.dylib" "$dir/B.dylib"
    # Link against the matching ARM interface when the installed B is x86-only.
    clang "${guest_arch[@]}" -fobjc-arc -framework Foundation -dynamiclib "$out/G.m" \
        -Wl,-needed_library,"$out/native/B.dylib" -install_name @rpath/G.framework/Versions/A/G -o "$fw/Versions/A/G"
    codesign -fs - "$fw"
    clang "${guest_arch[@]}" "$out/main.c" -Wl,-needed_library,"$out/native/A.dylib" -Wl,-needed_library,"$fw/G" \
        -Wl,-rpath,@executable_path -o "$dir/main"
    codesign -fs - "$dir/main"
done
printf '%s\n' A 'B framework=1 class=1 value=72' 'racing typed return 1' 'replay 1' H 'worker 72' 'G class load' G > "$out/expected"
CTOR_ORDER=GH CTOR_NESTED="$out/native/H.dylib" timeout -k 2 20 "$out/native/main" > "$out/native.out"
diff -u "$out/expected" "$out/native.out"
CTOR_ORDER=HG CTOR_CHECK_DEFERRED=1 CTOR_NESTED="$out/guest/H.dylib" timeout -k 2 20 "$hao" "$out/guest/main" > "$out/guest.out"
diff -u "$out/native.out" "$out/guest.out"
# An unrealized external superclass cannot be realized inside the mapping
# notifier. G follows its ordinary initialization path once B returns.
for variant in deferred missing tampered unsigned ignored nativeclass; do
    mkdir -p "$out/$variant"
    cp -R "$out/guest/." "$out/$variant/"
done
fw="$out/deferred/G.framework"
clang -arch arm64 -DEXTERNAL_SUPER -fobjc-arc -framework Foundation -dynamiclib "$out/G.m" \
    "$out/native/A.dylib" -Wl,-needed_library,"$out/native/B.dylib" -install_name @rpath/G.framework/Versions/A/G -o "$fw/Versions/A/G"
codesign -fs - "$fw"
# Link one ordinary class against an unavailable weak Objective-C superclass.
# Normal registration clears the classref before sealing. Early publication
# must defer because a racing caller could observe the unresolved reference.
fw="$out/ignored/G.framework"
cat > "$out/ignored-ref.s" <<'SOURCE'
.section __DATA_CONST,__objc_classrefs,regular,no_dead_strip
.p2align 3
.globl _fixture_ignored_ref
_fixture_ignored_ref:
.quad _OBJC_CLASS_$_CtorIgnoredClass
SOURCE
clang -arch arm64 -DMISSING_WEAK_SUPER -fobjc-arc -framework Foundation -dynamiclib "$out/G.m" "$out/ignored-ref.s" \
    -Wl,-undefined,dynamic_lookup \
    -Wl,-needed_library,"$out/native/B.dylib" -install_name @rpath/G.framework/Versions/A/G -o "$fw/Versions/A/G"
codesign -fs - "$fw"
CTOR_CHECK_DEFERRED=1 CTOR_NESTED="$out/ignored/H.dylib" timeout -k 2 20 "$hao" "$out/ignored/main" > "$out/ignored.out"
grep -qx 'B framework=0 class=0 value=-1' "$out/ignored.out"
grep -qx 'ignored reference nil' "$out/ignored.out"
# B defines an Objective-C class. A method of G could look it up by name
# while dyld has mapped B but libobjc has not registered B's classes.
dir="$out/nativeclass"
clang -arch x86_64 -DOWN_CLASS -fobjc-arc -framework Foundation -dynamiclib "$out/B.m" "$dir/A.dylib" -install_name @rpath/B.dylib -o "$dir/B.dylib"
codesign -fs - "$dir/B.dylib"
CTOR_NESTED="$dir/H.dylib" timeout -k 2 20 "$hao" "$dir/main" > "$out/nativeclass.out"
grep -qx 'B framework=0 class=0 value=-1' "$out/nativeclass.out"
# Also exercise a statically bound native class, and check it after normal
# initialization. The linker uses an ARM interface for the x86 dependency.
clang -arch arm64 -DOWN_CLASS -fobjc-arc -framework Foundation -dynamiclib "$out/B.m" "$out/native/A.dylib" -install_name @rpath/B.dylib -o "$out/B-interface.dylib"
fw="$dir/G.framework"
clang -arch arm64 -DNATIVE_CLASS_REF -fobjc-arc -framework Foundation -dynamiclib "$out/G.m" "$out/B-interface.dylib" -install_name @rpath/G.framework/Versions/A/G -o "$fw/Versions/A/G"
codesign -fs - "$fw"
CTOR_NESTED="$dir/H.dylib" timeout -k 2 20 "$hao" "$dir/main" > "$out/nativeclass-ref.out"
grep -qx 'B framework=0 class=0 value=-1' "$out/nativeclass-ref.out"
grep -qx 'native B class ready' "$out/nativeclass-ref.out"
# The existing native-mapping verifier requires a signed x86 slice.
clang -arch x86_64 -Wl,-no_adhoc_codesign -fobjc-arc -framework Foundation -dynamiclib "$out/B.m" \
    "$out/unsigned/A.dylib" -install_name @rpath/B.dylib -o "$out/unsigned/B.dylib"
printf '%s\n' A 'B framework=0 class=0 value=-1' 'racing typed return 0' 'G class load' G > "$out/deferred.expected"
for variant in deferred unsigned; do
    CTOR_NESTED="$out/$variant/H.dylib" timeout -k 2 20 "$hao" "$out/$variant/main" > "$out/$variant.out"
    diff -u "$out/deferred.expected" "$out/$variant.out"
done
# B is admitted, but another required G edge was never resolved. Its
# constructor must see no published G class or framework.
clang -arch arm64 -dynamiclib "$out/missing.c" -install_name @rpath/Missing.dylib -o "$out/interface.dylib"
fw="$out/missing/G.framework"
clang -arch arm64 -fobjc-arc -framework Foundation -dynamiclib "$out/G.m" \
    -Wl,-needed_library,"$out/native/B.dylib" -Wl,-needed_library,"$out/interface.dylib" \
    -install_name @rpath/G.framework/Versions/A/G -o "$fw/Versions/A/G"
codesign -fs - "$fw"
# Corrupt only a harmless marker in this authored and previously signed B.
python3 - "$out/tampered/B.dylib" <<'PYTHON'
from pathlib import Path
import sys
path = Path(sys.argv[1])
data = path.read_bytes()
old = b"ctor-verification-marker"
assert data.count(old) == 1
path.write_bytes(data.replace(old, b"Ctor-verification-marker"))
PYTHON
if codesign --verify --strict "$out/tampered/B.dylib" 2>/dev/null; then exit 1; fi
for variant in missing tampered; do
    if CTOR_NESTED="$out/$variant/H.dylib" timeout -k 2 20 "$hao" "$out/$variant/main" > "$out/$variant.out" 2>&1; then
        # Some hosts admit an unsigned native image. Such admission cannot
        # qualify the modified CodeDirectory for early guest publication.
        test "$variant" = tampered
        grep -qx 'B framework=0 class=0 value=-1' "$out/$variant.out"
    else
        status=$?
        test "$status" -ne 124
        if [ "$variant" = tampered ]; then
            grep -Ei 'code signature.*(invalid|not valid)|signature.*not valid for use' "$out/$variant.out"
        fi
    fi
    if grep -Eq '^B framework=1|^worker 72' "$out/$variant.out"; then exit 1; fi
done
grep -q 'Missing.dylib' "$out/missing.out"
