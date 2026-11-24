#import <Foundation/Foundation.h>
#include <dlfcn.h>
#import <objc/runtime.h>
static NSString *aliasPath;
static id alias_bundle_path(id self, SEL selector) { return aliasPath; }
static void observe_alias(NSString *path) {
    NSBundle *bundle = [NSBundle bundleWithPath:path];
    if (!bundle) exit(6);
    /* Foundation may canonicalize the input. Preserve the alias spelling
     * in this fixture's subclass to exercise the loaded-state lookup itself. */
    Class base = object_getClass(bundle);
    Class alias = objc_allocateClassPair(base, "HAOFixtureAliasBundle", 0);
    if (!alias || !class_addMethod(alias, @selector(bundlePath), (IMP)alias_bundle_path, "@@:")) exit(7);
    objc_registerClassPair(alias);
    aliasPath = path;
    object_setClass(bundle, alias);
    if (![bundle.bundlePath isEqual:path]) exit(8);
    printf("aliasedBundleLoaded=%d\n", bundle.loaded);
    object_setClass(bundle, base);
}
static void observe(NSString *phase, NSString *path) {
    NSBundle *onDisk = getenv("PRECREATE") ? [NSBundle bundleWithPath:path] : nil;
    if (getenv("PRECREATE") && (!onDisk || !onDisk.executablePath)) exit(4);
    NSBundle *match = nil;
    for (NSBundle *bundle in NSBundle.allFrameworks) {
        if ([bundle.bundleIdentifier isEqual:@"org.hao.fixture.lifecycle"]) match = bundle;
    }
    NSBundle *identified = [NSBundle bundleWithIdentifier:@"org.hao.fixture.lifecycle"];

    if (identified && !identified.executablePath) exit(5);
    printf("%s enumerated=%d idFound=%d enumeratedLoaded=%d onDiskLoaded=%d identifierLoaded=%d\n",
           phase.UTF8String, !!match, !!identified, match.loaded, onDisk.loaded, identified.loaded);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        observe(@"before", path);
        void *image = dlopen([[path stringByAppendingPathComponent:@"Foo"] fileSystemRepresentation], RTLD_NOW | RTLD_LOCAL);
        if (!image) { fprintf(stderr,"%s\n",dlerror()); return 1; }
        int (*value)(void) = dlsym(image, "fixture_value");
        if (!value || value() != 74) return 3;
        observe(@"after", path);
        if (getenv("ALIAS_BUNDLE")) observe_alias(path);
    }
    return 0;
}
