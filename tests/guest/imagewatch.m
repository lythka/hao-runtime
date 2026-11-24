// A native observer reads a typed, static callback record from the main
// executable and from a subsequently opened ObjC image.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>

typedef struct {
    unsigned kind, reserved;
    bool (*read)(void *, const void *, const void *, uintptr_t);
    uintptr_t context, reserved2;
} Record;

#ifdef WATCHER
static int counts[2];
static int replay;
static void observe(const struct mach_header *header) {
    unsigned long size = 0;
    const Record *record = (const void *)getsectiondata((const void *)header, "__DATA_CONST", "__hao_watch", &size);
    if (!record || !size || record->kind == 2) return;
    int value = 0;
    if (size != sizeof *record || record->kind > 1 ||
        !record->read(&value, &record->context, NULL, 0) || value != 42) abort();
    counts[record->kind]++;
    printf("%s %u\n", replay ? "replay" : "image", record->kind);
}
// Hold registration's main-image replay open while the main thread opens
// the second image. The same observer must never be active on both threads.
static _Atomic bool replay_started, replay_finished, overlap;
static pthread_t registrar;
static void racing_observer(const struct mach_header *header) {
    unsigned long size = 0;
    const Record *record = (const void *)getsectiondata((const void *)header, "__DATA_CONST", "__hao_watch", &size);
    if (!record || size != sizeof *record) return;
    if (record->kind == 0) {
        atomic_store(&replay_started, true);
        usleep(250000);
        atomic_store(&replay_finished, true);
    } else if (!atomic_load(&replay_finished)) {
        atomic_store(&overlap, true);
    }
}
static void *register_racing_observer(void *arg) {
    objc_addLoadImageFunc(racing_observer);
    return NULL;
}
static const char *next_image;
static void nested_observer(const struct mach_header *header) {
    unsigned long size = 0;
    const Record *record = (const void *)getsectiondata((const void *)header, "__DATA_CONST", "__hao_watch", &size);
    if (!record || size != sizeof *record) return;
    printf("nested %u\n", record->kind);
    if (record->kind == 0 && !dlopen(next_image, RTLD_NOW)) abort();
}
void watch_nested(const char *path) {
    next_image = path;
    objc_addLoadImageFunc(nested_observer);
}
void watch_begin(void) {
    objc_addLoadImageFunc(observe);
    if (pthread_create(&registrar, NULL, register_racing_observer, NULL)) abort();
    while (!atomic_load(&replay_started)) usleep(1000);
}
int watch_complete(void) {
    if (pthread_join(registrar, NULL) || atomic_load(&overlap)) return 0;
    if (counts[0] != 1 || counts[1] != 1) return 0;
    replay = 1;
    objc_addLoadImageFunc(observe);
    return counts[0] == 2 && counts[1] == 2;
}
#else
static bool read_value(void *out, const void *type, const void *hint, uintptr_t reserved) {
    if (hint || reserved || *(const uintptr_t *)type != 42) return false;
    *(int *)out = 42;
    return true;
}
__attribute__((used, section("__DATA_CONST,__hao_watch")))
static const Record record = {IMAGE_KIND, 0, read_value, 42, 0};
#if IMAGE_KIND == 0
extern void watch_begin(void);
extern int watch_complete(void);
extern void watch_nested(const char *);
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    watch_begin();
    if (!dlopen(argv[1], RTLD_NOW)) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    if (!watch_complete()) return 1;
    if (argc == 3) watch_nested(argv[2]);
    return 0;
}
#elif IMAGE_KIND == 1
@interface HaoImageWatchClass : NSObject @end
@implementation HaoImageWatchClass
+ (void)load { puts("class load"); }
@end
#endif
#endif
