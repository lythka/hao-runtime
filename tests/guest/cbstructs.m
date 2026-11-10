/* Native APIs that receive structures of guest callbacks (src/callbacks.c):
 * rb_tree ops, CF collection callbacks, CFRuntimeClass, CFAllocatorContext,
 * run-loop source/timer/observer/port/stream contexts, SystemConfiguration,
 * FSEvents and CFNetwork contexts, CG/CT callback tables, AudioUnit render
 * callbacks, NSMapTable/NSHashTable callbacks, sqlite3 modules and the zlib,
 * bzip2 and xz allocators. Structures live in __DATA, __DATA_CONST and on the
 * stack, the places real programs keep them. Build and run with
 * tests/guest/callbacks.sh; compare arm64e under hao with native arm64.
 * Optional argv[1]: run one section (1..10). */
#import <Foundation/Foundation.h>
#include <Block.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CFNetwork/CFNetwork.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <CoreText/CoreText.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <bzlib.h>
#include <mach/mach.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/rbtree.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static const char *YN(long v) { return v ? "yes" : "no"; }
static const char *cfs(CFStringRef s)
{
    static char buf[256];
    if (!s || !CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return "(null)";
    return buf;
}
static int tag;
static void *const TAG = &tag;

/* five-field context callbacks, shared by every context API */
static int n_cret, n_crel, n_cdesc;
static const void *c_retain(const void *info) { if (info == TAG) n_cret++; return info; }
static void c_release(const void *info) { if (info == TAG) n_crel++; }
static CFStringRef c_desc(const void *info) { n_cdesc++; return CFStringCreateWithCString(NULL, "hao-ctx", kCFStringEncodingUTF8); }
/* the same, for contexts declared without const (CFFileDescriptor, CFStream) */
static void *v_retain(void *info) { return (void *)c_retain(info); }
static void v_release(void *info) { c_release(info); }
static CFStringRef v_desc(void *info) { return c_desc(info); }
static void ctx_reset(void) { n_cret = n_crel = n_cdesc = 0; }
static void ctx_report(const char *what) { printf("%s: retain %d release %d desc %d\n", what, n_cret, n_crel, n_cdesc); ctx_reset(); }

/* ------------------------------------------------------------ 1: rb_tree */
struct rbnode { rb_node_t rb; int key; };
static int n_rbnodes, n_rbkey;
static int rb_nodes(void *ctx, const void *a, const void *b)
{
    n_rbnodes++;
    int x = ((const struct rbnode *)a)->key, y = ((const struct rbnode *)b)->key;
    return x < y ? -1 : x > y;
}
static int rb_key(void *ctx, const void *n, const void *k)
{
    n_rbkey++;
    int x = ((const struct rbnode *)n)->key, y = *(const int *)k;
    return x < y ? -1 : x > y;
}
static rb_tree_ops_t rb_ops_data = {rb_nodes, rb_key, offsetof(struct rbnode, rb), NULL};          /* __DATA */
static const rb_tree_ops_t rb_ops_const = {rb_nodes, rb_key, offsetof(struct rbnode, rb), NULL};   /* __DATA_CONST */

static void rb_run(const char *what, const rb_tree_ops_t *ops)
{
    enum { N = 300 };
    static struct rbnode nodes[N];
    rb_tree_t tree;
    n_rbnodes = n_rbkey = 0;
    rb_tree_init(&tree, ops);
    for (int k = 0; k < N; k++) { nodes[k].key = (k * 37) % 311; rb_tree_insert_node(&tree, &nodes[k]); }
    int found = 0;
    for (int k = 0; k < 311; k += 3) if (rb_tree_find_node(&tree, &k)) found++;
    for (int k = 0; k < N; k += 4) rb_tree_remove_node(&tree, &nodes[k]);
    long sum = 0, pos = 0, prev = -1, ordered = 1;
    for (struct rbnode *n = rb_tree_iterate(&tree, NULL, RB_DIR_LEFT); n; n = rb_tree_iterate(&tree, n, RB_DIR_RIGHT)) {
        sum += ++pos * n->key;
        if (n->key <= prev) ordered = 0;
        prev = n->key;
    }
    int k = 100;
    struct rbnode *geq = rb_tree_find_node_geq(&tree, &k);
    printf("%s: found %d, %ld left, weighted sum %ld, ordered %s, geq(100) %d, comparators called %s/%s\n", what, found, pos,
           sum, YN(ordered), geq ? geq->key : -1, YN(n_rbnodes), YN(n_rbkey));
}

static void t1(void)
{
    rb_run("rb data ops", &rb_ops_data);
    rb_run("rb const ops", &rb_ops_const);
    rb_tree_ops_t stack_ops = rb_ops_const;
    rb_run("rb stack ops", &stack_ops);
}

/* ------------------------------------------------------- 2: CF collections */
static int n_ret, n_rel, n_desc, n_eq, n_hash;
static const void *s_retain(CFAllocatorRef a, const void *v) { n_ret++; return v; }
static void s_release(CFAllocatorRef a, const void *v) { n_rel++; }
static CFStringRef s_desc(const void *v) { n_desc++; return CFStringCreateWithCString(NULL, v, kCFStringEncodingUTF8); }
static Boolean s_equal(const void *a, const void *b) { n_eq++; return strcasecmp(a, b) == 0; }
static CFHashCode s_hash(const void *v)
{
    n_hash++;
    CFHashCode h = 5381;
    for (const char *p = v; *p; p++) h = h * 33 + (unsigned char)(*p | 0x20);
    return h;
}
static void cb_reset(void) { n_ret = n_rel = n_desc = n_eq = n_hash = 0; }
static void cb_report(const char *what)
{
    printf("%s: retain=release %s, equal %s, hash %s, desc %s\n", what, YN(n_ret && n_ret == n_rel), YN(n_eq), YN(n_hash), YN(n_desc));
    cb_reset();
}

static const CFDictionaryKeyCallBacks dkey_const = {0, s_retain, s_release, s_desc, s_equal, s_hash};
static CFDictionaryValueCallBacks dval_data = {0, s_retain, s_release, s_desc, s_equal};
static const CFSetCallBacks set_const = {0, s_retain, s_release, s_desc, s_equal, s_hash};
static const CFBagCallBacks bag_const = {0, s_retain, s_release, s_desc, s_equal, s_hash};
static CFArrayCallBacks arr_data = {0, s_retain, s_release, s_desc, s_equal};

static Boolean cfci_equal(const void *a, const void *b) { n_eq++; return CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo; }
static CFHashCode cfci_hash(const void *v) { n_hash++; return (CFHashCode)CFStringGetLength(v); }

static CFComparisonResult heap_cmp(const void *a, const void *b, void *info)
{
    n_eq++;
    long x = (long)a, y = (long)b;
    return x > y ? kCFCompareLessThan : x < y ? kCFCompareGreaterThan : kCFCompareEqualTo;   /* max-heap */
}
static const void *heap_retain(CFAllocatorRef a, const void *v) { n_ret++; return v; }
static void heap_release(CFAllocatorRef a, const void *v) { n_rel++; }
static CFBinaryHeapCallBacks heap_cb = {0, heap_retain, heap_release, NULL, heap_cmp};

static void t2(void)
{
    cb_reset();
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &dkey_const, &dval_data);
    CFDictionarySetValue(d, "Alpha", "one");
    CFDictionarySetValue(d, "BETA", "two");
    CFDictionarySetValue(d, "alpha", "uno");
    CFDictionarySetValue(d, "gamma", "three");
    printf("dict mutable: count %ld, ALPHA -> %s, beta -> %s, has GAMMA %s\n", (long)CFDictionaryGetCount(d),
           (const char *)CFDictionaryGetValue(d, "ALPHA"), (const char *)CFDictionaryGetValue(d, "beta"),
           YN(CFDictionaryContainsKey(d, "GAMMA")));
    printf("dict contains value TWO: %s\n", YN(CFDictionaryContainsValue(d, "TWO")));
    CFStringRef ds = CFCopyDescription(d);
    CFRelease(ds);
    CFRelease(d);
    cb_report("dict mutable");

    const void *keys[] = {"x", "Y", "z"}, *vals[] = {"1", "2", "3"};
    CFDictionaryRef di = CFDictionaryCreate(NULL, keys, vals, 3, &dkey_const, &dval_data);
    printf("dict: y -> %s, X -> %s\n", (const char *)CFDictionaryGetValue(di, "y"), (const char *)CFDictionaryGetValue(di, "X"));
    CFRelease(di);
    n_desc = 1;
    cb_report("dict");

    /* the common pattern: kCFType callbacks copied to the stack, equal/hash overridden */
    CFDictionaryKeyCallBacks kc = kCFTypeDictionaryKeyCallBacks;
    kc.equal = cfci_equal;
    kc.hash = cfci_hash;
    const void *ck[] = {CFSTR("Key"), CFSTR("other")}, *cv[] = {CFSTR("v1"), CFSTR("v2")};
    CFDictionaryRef dk = CFDictionaryCreate(NULL, ck, cv, 2, &kc, &kCFTypeDictionaryValueCallBacks);
    printf("dict stack callbacks: KEY -> %s", cfs(CFDictionaryGetValue(dk, CFSTR("KEY"))));
    printf(", OTHER -> %s\n", cfs(CFDictionaryGetValue(dk, CFSTR("OTHER"))));
    CFRelease(dk);
    printf("dict stack callbacks: equal %s, hash %s\n", YN(n_eq), YN(n_hash));
    cb_reset();

    /* native callbacks stay native */
    CFMutableDictionaryRef dn = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dn, CFSTR("a"), CFSTR("b"));
    printf("dict native callbacks: a -> %s\n", cfs(CFDictionaryGetValue(dn, CFSTR("a"))));
    CFRelease(dn);

    CFMutableSetRef s = CFSetCreateMutable(NULL, 0, &set_const);
    CFSetAddValue(s, "x"); CFSetAddValue(s, "X"); CFSetAddValue(s, "y");
    printf("set mutable: count %ld, contains Y %s\n", (long)CFSetGetCount(s), YN(CFSetContainsValue(s, "Y")));
    CFRelease(s);
    n_desc = 1;
    cb_report("set mutable");

    CFSetCallBacks sc = kCFTypeSetCallBacks;
    sc.equal = cfci_equal;
    sc.hash = cfci_hash;
    const void *sv[] = {CFSTR("ab"), CFSTR("AB"), CFSTR("abc")};
    CFSetRef si = CFSetCreate(NULL, sv, 3, &sc);
    printf("set stack callbacks: count %ld, contains aBc %s\n", (long)CFSetGetCount(si), YN(CFSetContainsValue(si, CFSTR("aBc"))));
    CFRelease(si);
    printf("set stack callbacks: equal %s, hash %s\n", YN(n_eq), YN(n_hash));
    cb_reset();

    CFMutableBagRef b = CFBagCreateMutable(NULL, 0, &bag_const);
    CFBagAddValue(b, "a"); CFBagAddValue(b, "A"); CFBagAddValue(b, "b");
    printf("bag mutable: count %ld, count of a %ld\n", (long)CFBagGetCount(b), (long)CFBagGetCountOfValue(b, "a"));
    CFRelease(b);
    n_desc = 1;
    cb_report("bag mutable");
    const void *bv[] = {"q", "Q", "r"};
    CFBagRef bi = CFBagCreate(NULL, bv, 3, &bag_const);
    printf("bag: count of q %ld\n", (long)CFBagGetCountOfValue(bi, "q"));
    CFRelease(bi);
    n_desc = 1;
    cb_report("bag");

    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &arr_data);
    CFArrayAppendValue(a, "a"); CFArrayAppendValue(a, "b"); CFArrayAppendValue(a, "c");
    printf("array mutable: index of B %ld, contains C %s\n", (long)CFArrayGetFirstIndexOfValue(a, CFRangeMake(0, 3), "B"),
           YN(CFArrayContainsValue(a, CFRangeMake(0, 3), "C")));
    CFArrayRemoveValueAtIndex(a, 0);
    CFRelease(a);
    n_hash = n_desc = 1;
    cb_report("array mutable");
    const void *av[] = {"p", "q"};
    CFArrayRef ai = CFArrayCreate(NULL, av, 2, &arr_data);
    printf("array: index of Q %ld\n", (long)CFArrayGetFirstIndexOfValue(ai, CFRangeMake(0, 2), "Q"));
    CFRelease(ai);
    n_hash = n_desc = 1;
    cb_report("array");

    ctx_reset();
    CFBinaryHeapCompareContext hc = {0, TAG, c_retain, c_release, c_desc};
    CFBinaryHeapRef h = CFBinaryHeapCreate(NULL, 0, &heap_cb, &hc);
    long in[] = {5, 3, 9, 1, 7};
    for (int k = 0; k < 5; k++) CFBinaryHeapAddValue(h, (const void *)in[k]);
    printf("binary heap: max first:");
    while (CFBinaryHeapGetCount(h)) { printf(" %ld", (long)CFBinaryHeapGetMinimum(h)); CFBinaryHeapRemoveMinimumValue(h); }
    printf("\n");
    CFRelease(h);
    n_hash = n_desc = 1;
    cb_report("binary heap");
    ctx_report("binary heap context");
}

/* ------------------------------------ 3: CFRuntimeClass, CFAllocatorContext */
typedef struct {
    CFIndex version;
    const char *className;
    void (*init)(CFTypeRef cf);
    CFTypeRef (*copy)(CFAllocatorRef allocator, CFTypeRef cf);
    void (*finalize)(CFTypeRef cf);
    Boolean (*equal)(CFTypeRef cf1, CFTypeRef cf2);
    CFHashCode (*hash)(CFTypeRef cf);
    CFStringRef (*copyFormattingDesc)(CFTypeRef cf, CFDictionaryRef formatOptions);
    CFStringRef (*copyDebugDesc)(CFTypeRef cf);
    void (*reclaim)(CFTypeRef cf);
    uint32_t (*refcount)(intptr_t op, CFTypeRef cf);
    uintptr_t requiredAlignment;
} RuntimeClass;
CFTypeID _CFRuntimeRegisterClass(const RuntimeClass *cls);
CFTypeRef _CFRuntimeCreateInstance(CFAllocatorRef allocator, CFTypeID typeID, CFIndex extraBytes, unsigned char *category);

typedef struct { uintptr_t isa; uint64_t info; long value; } Thing;   /* CFRuntimeBase + payload */
static int n_init, n_fin, n_teq, n_thash, n_tdesc;
static void th_init(CFTypeRef cf) { n_init++; }
static void th_finalize(CFTypeRef cf) { n_fin++; }
static Boolean th_equal(CFTypeRef a, CFTypeRef b) { n_teq++; return ((const Thing *)a)->value % 10 == ((const Thing *)b)->value % 10; }
static CFHashCode th_hash(CFTypeRef cf) { n_thash++; return (CFHashCode)(((const Thing *)cf)->value % 10); }
static CFStringRef th_desc(CFTypeRef cf)
{
    n_tdesc++;
    char buf[32];
    snprintf(buf, sizeof buf, "<HaoThing %ld>", ((const Thing *)cf)->value);
    return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}
static const RuntimeClass thing_class = {0, "HaoThing", th_init, NULL, th_finalize, th_equal, th_hash, NULL, th_desc, NULL, NULL, 0};

static int n_alloc, n_realloc, n_free;
static void *al_alloc(CFIndex size, CFOptionFlags hint, void *info) { if (info == TAG) n_alloc++; return malloc((size_t)size); }
static void *al_realloc(void *p, CFIndex size, CFOptionFlags hint, void *info) { if (info == TAG) n_realloc++; return realloc(p, (size_t)size); }
static void al_free(void *p, void *info) { if (info == TAG) n_free++; free(p); }
static CFIndex al_pref(CFIndex size, CFOptionFlags hint, void *info) { return size; }

static void t3(void)
{
    CFTypeID id = _CFRuntimeRegisterClass(&thing_class);
    Thing *a = (Thing *)_CFRuntimeCreateInstance(NULL, id, sizeof(long), NULL);
    Thing *b = (Thing *)_CFRuntimeCreateInstance(NULL, id, sizeof(long), NULL);
    a->value = 13;
    b->value = 23;
    printf("runtime class: equal %s, hash %lu, type %s\n", YN(CFEqual(a, b)), (unsigned long)CFHash(a), YN(CFGetTypeID(a) == id));
    CFStringRef d = CFCopyDescription(a);
    printf("runtime class: description %s\n", cfs(d));
    CFRelease(d);
    const void *v[] = {a, b};
    CFSetRef s = CFSetCreate(NULL, v, 2, &kCFTypeSetCallBacks);
    printf("runtime class: set of equal things has %ld\n", (long)CFSetGetCount(s));
    CFRelease(s);
    CFRelease(a);
    CFRelease(b);
    printf("runtime class: init %d finalize %d, equal %s hash %s desc %s\n", n_init, n_fin, YN(n_teq), YN(n_thash), YN(n_tdesc));

    ctx_reset();
    CFAllocatorContext ac = {0, TAG, c_retain, c_release, c_desc, al_alloc, al_realloc, al_free, al_pref};
    CFAllocatorRef al = CFAllocatorCreate(NULL, &ac);
    uint8_t bytes[300];
    for (int k = 0; k < 300; k++) bytes[k] = (uint8_t)k;
    CFDataRef data = CFDataCreate(al, bytes, sizeof bytes);
    printf("allocator: data length %ld, byte 299 = %d\n", (long)CFDataGetLength(data), CFDataGetBytePtr(data)[299]);
    CFRelease(data);
    void *p = CFAllocatorAllocate(al, 64, 0);
    p = CFAllocatorReallocate(al, p, 4096, 0);
    CFAllocatorDeallocate(al, p);
    char *buf = malloc(16);
    strcpy(buf, "no-copy string");
    CFStringRef nc = CFStringCreateWithBytesNoCopy(NULL, (const UInt8 *)buf, (CFIndex)strlen(buf), kCFStringEncodingUTF8, false, al);
    printf("allocator: no-copy string %s\n", cfs(nc));
    CFRelease(nc);
    CFStringRef ad = CFCopyDescription(al);
    CFRelease(ad);
    CFRelease(al);
    printf("allocator: allocate %s, reallocate %s, all freed %s\n", YN(n_alloc), YN(n_realloc), YN(n_alloc && n_free == n_alloc + 1));
    ctx_report("allocator context");
}

/* ------------------------------------- 4: run-loop sources, timers, ports */
static int n_sched, n_cancel, n_perform, n_seq, n_shash, n_getport, n_v1perform, n_fire, n_obs, n_fd, n_sock, n_stream;
static mach_port_t v1_port;
static void src_schedule(void *info, CFRunLoopRef rl, CFRunLoopMode mode) { n_sched++; }
static void src_cancel(void *info, CFRunLoopRef rl, CFRunLoopMode mode) { n_cancel++; }
static void src_perform(void *info) { n_perform++; CFRunLoopStop(CFRunLoopGetCurrent()); }
static Boolean src_equal(const void *a, const void *b) { n_seq++; return a == b; }
static CFHashCode src_hash(const void *info) { n_shash++; return 77; }
static mach_port_t v1_getport(void *info) { n_getport++; return v1_port; }
static void *v1_perform(void *msg, CFIndex size, CFAllocatorRef al, void *info)
{
    n_v1perform++;
    printf("source1: message id %d size %s\n", ((mach_msg_header_t *)msg)->msgh_id, YN(size >= (CFIndex)sizeof(mach_msg_header_t)));
    CFRunLoopStop(CFRunLoopGetCurrent());
    return NULL;
}
static CFRunLoopTimerRef expected_timer;
static CFRunLoopObserverRef expected_observer;
static unsigned runloop_arg_errors;
static CFRunLoopActivity observer_activities;
static volatile uint64_t void_result_canary = UINT64_C(0x123456789abcdef0);
static inline void leave_void_result_canary(void)
{
    /* A void callback has no indirect result: leaving a real address in x8
     * must not cause the bridge to write a result there. This checks writes,
     * not whether a generic bridge speculatively reads the address. */
#if defined(__arm64__)
    __asm__ volatile("mov x8, %0" : : "r"(&void_result_canary) : "x8");
#endif
}
static void tm_fire(CFRunLoopTimerRef t, void *info)
{
    if (t != expected_timer || info != TAG) runloop_arg_errors++;
    n_fire++; CFRunLoopStop(CFRunLoopGetCurrent());
    leave_void_result_canary();
}
static void obs_cb(CFRunLoopObserverRef o, CFRunLoopActivity act, void *info)
{
    if (o != expected_observer || info != TAG ||
        (act != kCFRunLoopEntry && act != kCFRunLoopExit)) runloop_arg_errors++;
    observer_activities |= act;
    n_obs++;
    leave_void_result_canary();
}
static void mp_cb(CFMachPortRef p, void *msg, CFIndex size, void *info) {}
static CFDataRef msgp_cb(CFMessagePortRef p, SInt32 id, CFDataRef data, void *info) { return NULL; }
static void fd_cb(CFFileDescriptorRef f, CFOptionFlags types, void *info) { n_fd++; CFRunLoopStop(CFRunLoopGetCurrent()); }
static void sock_cb(CFSocketRef s, CFSocketCallBackType type, CFDataRef addr, const void *data, void *info)
{
    n_sock++;
    CFRunLoopStop(CFRunLoopGetCurrent());
}
static char rs_buf[64];
static size_t rs_len;
static void rs_cb(CFReadStreamRef s, CFStreamEventType ev, void *info)
{
    n_stream++;
    if (ev == kCFStreamEventHasBytesAvailable) {
        CFIndex n = CFReadStreamRead(s, (UInt8 *)rs_buf + rs_len, (CFIndex)(sizeof rs_buf - 1 - rs_len));
        if (n > 0) rs_len += (size_t)n;
    }
    if (ev == kCFStreamEventEndEncountered || ev == kCFStreamEventErrorOccurred) CFRunLoopStop(CFRunLoopGetCurrent());
}
static void ws_cb(CFWriteStreamRef s, CFStreamEventType ev, void *info) {}

static void spin(double s) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, s, false); }
static void describe(CFTypeRef o) { CFStringRef d = CFCopyDescription(o); if (d) CFRelease(d); }

static void t4(void)
{
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    ctx_reset();
    CFRunLoopSourceContext sc = {0, TAG, c_retain, c_release, c_desc, src_equal, src_hash, src_schedule, src_cancel, src_perform};
    CFRunLoopSourceRef s0 = CFRunLoopSourceCreate(NULL, 0, &sc);
    CFRunLoopSourceRef s0b = CFRunLoopSourceCreate(NULL, 0, &sc);
    printf("source0: equal %s, hash %lu\n", YN(CFEqual(s0, s0b)), (unsigned long)CFHash(s0));
    describe(s0);
    CFRunLoopAddSource(rl, s0, kCFRunLoopDefaultMode);
    CFRunLoopSourceSignal(s0);
    CFRunLoopWakeUp(rl);
    spin(2.0);
    CFRunLoopSourceInvalidate(s0);
    CFRelease(s0);
    CFRelease(s0b);
    printf("source0: schedule %d perform %d cancel %d, equal %s hash %s\n", n_sched, n_perform, n_cancel, YN(n_seq), YN(n_shash));
    ctx_report("source0 context");

    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &v1_port);
    mach_port_insert_right(mach_task_self(), v1_port, v1_port, MACH_MSG_TYPE_MAKE_SEND);
    CFRunLoopSourceContext1 sc1 = {1, TAG, c_retain, c_release, c_desc, src_equal, src_hash, v1_getport, v1_perform};
    CFRunLoopSourceRef s1 = CFRunLoopSourceCreate(NULL, 0, (CFRunLoopSourceContext *)&sc1);
    CFRunLoopAddSource(rl, s1, kCFRunLoopDefaultMode);
    mach_msg_header_t m = {0};
    m.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.msgh_size = sizeof m;
    m.msgh_remote_port = v1_port;
    m.msgh_id = 4242;
    kern_return_t kr = mach_msg(&m, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    spin(2.0);
    CFRunLoopSourceInvalidate(s1);
    CFRelease(s1);
    printf("source1: send %s, getPort %s, perform %d\n", YN(kr == KERN_SUCCESS), YN(n_getport), n_v1perform);
    ctx_report("source1 context");
    mach_port_mod_refs(mach_task_self(), v1_port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), v1_port);

    CFRunLoopTimerContext tc = {0, TAG, c_retain, c_release, c_desc};
    CFRunLoopTimerRef tm = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.01, 0, 0, 0, tm_fire, &tc);
    CFRunLoopObserverContext oc = {0, TAG, c_retain, c_release, c_desc};
    CFRunLoopObserverRef ob = CFRunLoopObserverCreate(NULL, kCFRunLoopEntry | kCFRunLoopExit, true, 0, obs_cb, &oc);
    expected_timer = tm;
    expected_observer = ob;
    /* Exercise an initially untyped block, then its authoritative descriptor.
     * A plain first registry entry must not hide the later void(arg,arg) type. */
    struct ObserverBlockHeader { void *isa; int flags, reserved; void *invoke; void *descriptor; };
    enum { HAS_SIGNATURE = 1 << 30 };
    __block CFRunLoopObserverRef block_observer = NULL;
    __block unsigned block_calls = 0, block_errors = 0;
    __block CFRunLoopActivity block_activities = 0;
    const void *captured_tag = TAG;
    void (^handler)(CFRunLoopObserverRef, CFRunLoopActivity) = ^(CFRunLoopObserverRef o, CFRunLoopActivity activity) {
        if (o != block_observer || captured_tag != TAG ||
            (activity != kCFRunLoopEntry && activity != kCFRunLoopExit)) block_errors++;
        block_calls++;
        block_activities |= activity;
        leave_void_result_canary();
    };
    struct ObserverBlockHeader *header = (void *)handler;
    header->flags &= ~HAS_SIGNATURE;
    handler = Block_copy(handler);
    header = (void *)handler;
    void *plain_first = header->invoke;
    header->flags |= HAS_SIGNATURE;
    block_observer = CFRunLoopObserverCreateWithHandler(NULL, kCFRunLoopEntry | kCFRunLoopExit,
                                                       true, 0, handler);
    if (!block_observer) abort();
    if (getenv("HAO_EXPECT_TYPED_CF_BLOCK") && header->invoke == plain_first) abort();
    CFRunLoopAddObserver(rl, block_observer, kCFRunLoopDefaultMode);
    CFRunLoopAddTimer(rl, tm, kCFRunLoopDefaultMode);
    CFRunLoopAddObserver(rl, ob, kCFRunLoopDefaultMode);
    describe(tm);
    describe(ob);
    spin(2.0);
    CFRunLoopRemoveObserver(rl, block_observer, kCFRunLoopDefaultMode);
    CFRunLoopObserverInvalidate(block_observer);
    CFRelease(block_observer);
    Block_release(handler);
    bool block_ok = block_calls && !block_errors &&
                    block_activities == (kCFRunLoopEntry | kCFRunLoopExit);
    printf("observer block args %s, captured context %s\n", YN(block_ok), YN(captured_tag == TAG));
    if (!block_ok) abort();
    CFRunLoopRemoveObserver(rl, ob, kCFRunLoopDefaultMode);
    CFRunLoopObserverInvalidate(ob);
    CFRunLoopTimerInvalidate(tm);
    CFRelease(ob);
    CFRelease(tm);
    printf("timer fired %d, observer called %s\n", n_fire, YN(n_obs));
    bool args_ok = runloop_arg_errors == 0 && observer_activities == (kCFRunLoopEntry | kCFRunLoopExit);
    bool void_ok = void_result_canary == UINT64_C(0x123456789abcdef0);
    printf("runloop callback args %s, void result untouched %s\n", YN(args_ok), YN(void_ok));
    if (!args_ok || !void_ok) abort();
    ctx_report("timer+observer contexts");

    Boolean shouldFree = false;
    CFMachPortContext mc = {0, TAG, c_retain, c_release, c_desc};
    CFMachPortRef mp = CFMachPortCreate(NULL, mp_cb, &mc, &shouldFree);
    describe(mp);
    mach_port_t raw;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &raw);
    CFMachPortRef mp2 = CFMachPortCreateWithPort(NULL, raw, mp_cb, &mc, &shouldFree);
    printf("mach ports: created %s %s\n", YN(mp != NULL), YN(mp2 != NULL));
    CFMachPortInvalidate(mp);
    CFMachPortInvalidate(mp2);
    CFRelease(mp);
    CFRelease(mp2);
    mach_port_mod_refs(mach_task_self(), raw, MACH_PORT_RIGHT_RECEIVE, -1);
    ctx_report("mach port contexts");

    char nm[64];
    snprintf(nm, sizeof nm, "org.hao.cbstructs.%d", (int)getpid());
    CFStringRef name = CFStringCreateWithCString(NULL, nm, kCFStringEncodingUTF8);
    CFMessagePortContext pc = {0, TAG, c_retain, c_release, c_desc};
    CFMessagePortRef msgp = CFMessagePortCreateLocal(NULL, name, msgp_cb, &pc, &shouldFree);
    printf("message port: created %s\n", YN(msgp != NULL));
    if (msgp) { describe(msgp); CFMessagePortInvalidate(msgp); CFRelease(msgp); }
    CFRelease(name);
    ctx_report("message port context");

    int fds[2];
    pipe(fds);
    CFFileDescriptorContext fc = {0, TAG, v_retain, v_release, v_desc};
    CFFileDescriptorRef fdr = CFFileDescriptorCreate(NULL, fds[0], false, fd_cb, &fc);
    CFFileDescriptorEnableCallBacks(fdr, kCFFileDescriptorReadCallBack);
    CFRunLoopSourceRef fsrc = CFFileDescriptorCreateRunLoopSource(NULL, fdr, 0);
    CFRunLoopAddSource(rl, fsrc, kCFRunLoopDefaultMode);
    write(fds[1], "x", 1);
    spin(2.0);
    describe(fdr);
    CFRunLoopRemoveSource(rl, fsrc, kCFRunLoopDefaultMode);
    CFRelease(fsrc);
    CFFileDescriptorInvalidate(fdr);
    CFRelease(fdr);
    close(fds[0]);
    close(fds[1]);
    printf("file descriptor: callback %d\n", n_fd);
    ctx_report("file descriptor context");

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CFSocketContext so = {0, TAG, c_retain, c_release, c_desc};
    CFSocketRef sk = CFSocketCreateWithNative(NULL, sv[0], kCFSocketReadCallBack, sock_cb, &so);
    CFRunLoopSourceRef ssrc = CFSocketCreateRunLoopSource(NULL, sk, 0);
    CFRunLoopAddSource(rl, ssrc, kCFRunLoopDefaultMode);
    write(sv[1], "y", 1);
    spin(2.0);
    describe(sk);
    CFRunLoopRemoveSource(rl, ssrc, kCFRunLoopDefaultMode);
    CFRelease(ssrc);
    CFSocketInvalidate(sk);
    CFRelease(sk);
    close(sv[1]);
    CFSocketRef sk2 = CFSocketCreate(NULL, PF_INET, SOCK_DGRAM, IPPROTO_UDP, 0, NULL, &so);
    printf("sockets: read callback %d, second socket %s\n", n_sock, YN(sk2 != NULL));
    if (sk2) { CFSocketInvalidate(sk2); CFRelease(sk2); }
    ctx_report("socket contexts");

    static const char text[] = "stream client context";
    CFReadStreamRef rs = CFReadStreamCreateWithBytesNoCopy(NULL, (const UInt8 *)text, (CFIndex)strlen(text), kCFAllocatorNull);
    CFStreamClientContext cc = {0, TAG, v_retain, v_release, v_desc};
    CFReadStreamSetClient(rs, kCFStreamEventHasBytesAvailable | kCFStreamEventEndEncountered | kCFStreamEventErrorOccurred, rs_cb, &cc);
    CFReadStreamScheduleWithRunLoop(rs, rl, kCFRunLoopDefaultMode);
    CFReadStreamOpen(rs);
    spin(2.0);
    CFReadStreamUnscheduleFromRunLoop(rs, rl, kCFRunLoopDefaultMode);
    CFReadStreamSetClient(rs, 0, NULL, NULL);
    CFReadStreamClose(rs);
    CFRelease(rs);
    printf("read stream: events %s, read \"%s\"\n", YN(n_stream), rs_buf);
    CFWriteStreamRef ws = CFWriteStreamCreateWithAllocatedBuffers(NULL, NULL);
    CFWriteStreamSetClient(ws, kCFStreamEventCanAcceptBytes, ws_cb, &cc);
    CFWriteStreamSetClient(ws, 0, NULL, NULL);
    CFRelease(ws);
    ctx_report("stream contexts");
}

/* ------------------------------------ 5: SystemConfiguration, FSEvents, CFNetwork */
static void ds_cb(SCDynamicStoreRef s, CFArrayRef keys, void *info) {}
static void pref_cb(SCPreferencesRef p, SCPreferencesNotification n, void *info) {}
static void reach_cb(SCNetworkReachabilityRef r, SCNetworkReachabilityFlags f, void *info) {}
static void fs_cb(ConstFSEventStreamRef s, void *info, size_t n, void *paths, const FSEventStreamEventFlags *fl, const FSEventStreamEventId *ids) {}
static void host_cb(CFHostRef h, CFHostInfoType t, const CFStreamError *e, void *info) {}
static void ns_cb(CFNetServiceRef s, CFStreamError *e, void *info) {}

static void t5(void)
{
    ctx_reset();
    SCDynamicStoreContext dc = {0, TAG, c_retain, c_release, c_desc};
    SCDynamicStoreRef ds = SCDynamicStoreCreate(NULL, CFSTR("hao-cbstructs"), ds_cb, &dc);
    SCDynamicStoreRef ds2 = SCDynamicStoreCreateWithOptions(NULL, CFSTR("hao-cbstructs"), NULL, ds_cb, &dc);
    printf("dynamic store: created %s %s\n", YN(ds != NULL), YN(ds2 != NULL));
    if (ds) { describe(ds); CFRelease(ds); }
    if (ds2) CFRelease(ds2);
    n_cdesc = !!n_cdesc;
    ctx_report("dynamic store contexts");

    SCPreferencesRef pr = SCPreferencesCreate(NULL, CFSTR("hao-cbstructs"), NULL);
    SCPreferencesContext pc = {0, TAG, c_retain, c_release, c_desc};
    printf("preferences: callback set %s\n", YN(pr && SCPreferencesSetCallback(pr, pref_cb, &pc)));
    if (pr) { SCPreferencesSetCallback(pr, NULL, NULL); CFRelease(pr); }
    ctx_report("preferences context");

    SCNetworkReachabilityRef re = SCNetworkReachabilityCreateWithName(NULL, "localhost");
    SCNetworkReachabilityContext rc = {0, TAG, c_retain, c_release, c_desc};
    printf("reachability: callback set %s\n", YN(re && SCNetworkReachabilitySetCallback(re, reach_cb, &rc)));
    if (re) { SCNetworkReachabilitySetCallback(re, NULL, NULL); CFRelease(re); }
    ctx_report("reachability context");

    CFStringRef path = CFSTR("/private/tmp");
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&path, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext fc = {0, TAG, c_retain, c_release, c_desc};
    FSEventStreamRef fs = FSEventStreamCreate(NULL, fs_cb, &fc, paths, kFSEventStreamEventIdSinceNow, 1.0, kFSEventStreamCreateFlagNone);
    struct stat st;
    stat("/", &st);
    CFStringRef rel = CFSTR("private/tmp");
    CFArrayRef rpaths = CFArrayCreate(NULL, (const void **)&rel, 1, &kCFTypeArrayCallBacks);
    FSEventStreamRef fs2 = FSEventStreamCreateRelativeToDevice(NULL, fs_cb, &fc, st.st_dev, rpaths, kFSEventStreamEventIdSinceNow, 1.0, 0);
    printf("fsevents: created %s %s\n", YN(fs != NULL), YN(fs2 != NULL));
    if (fs) FSEventStreamRelease(fs);
    if (fs2) FSEventStreamRelease(fs2);
    CFRelease(paths);
    CFRelease(rpaths);
    ctx_report("fsevents contexts");

    CFHostRef host = CFHostCreateWithName(NULL, CFSTR("localhost"));
    CFHostClientContext hc = {0, TAG, c_retain, c_release, c_desc};
    printf("host: client set %s\n", YN(CFHostSetClient(host, host_cb, &hc)));
    CFHostSetClient(host, NULL, NULL);
    CFRelease(host);
    CFNetServiceRef ns = CFNetServiceCreate(NULL, CFSTR("local."), CFSTR("_hao._tcp"), CFSTR("hao"), 4242);
    CFNetServiceClientContext nc = {0, TAG, c_retain, c_release, c_desc};
    printf("net service: client set %s\n", YN(CFNetServiceSetClient(ns, ns_cb, &nc)));
    CFNetServiceSetClient(ns, NULL, NULL);
    CFRelease(ns);
    ctx_report("cfnetwork contexts");
}

/* ------------------------------------------------- 6: CoreGraphics, CoreText */
static uint8_t dp_bytes[256];
static int n_dpget, n_dpptr, n_dpunptr, n_dprel, n_put, n_dcrel, n_eval, n_frel, n_draw, n_prel, n_rddealloc, n_rdget;
static size_t dp_get(void *info, void *buf, off_t pos, size_t cnt)
{
    n_dpget++;
    if (pos >= 256) return 0;
    if (cnt > (size_t)(256 - pos)) cnt = (size_t)(256 - pos);
    memcpy(buf, dp_bytes + pos, cnt);
    return cnt;
}
static const void *dp_ptr(void *info) { n_dpptr++; return dp_bytes; }
static void dp_unptr(void *info, const void *p) { n_dpunptr++; }
static void dp_release(void *info) { if (info == TAG) n_dprel++; }
static const CGDataProviderDirectCallbacks dp_cb_get = {0, NULL, NULL, dp_get, dp_release};
static size_t dc_put(void *info, const void *buf, size_t n) { n_put++; return n; }
static void dc_release(void *info) { if (info == TAG) n_dcrel++; }
static void fn_eval(void *info, const CGFloat *in, CGFloat *out)
{
    n_eval++;
    bool left = in[0] < 0.5;
    out[0] = left; out[1] = 0; out[2] = !left; out[3] = 1;
}
static void fn_release(void *info) { if (info == TAG) n_frel++; }
static const CGFunctionCallbacks fn_cb = {0, fn_eval, fn_release};
static void pat_draw(void *info, CGContextRef c)
{
    n_draw++;
    CGContextSetRGBFillColor(c, 0, 1, 0, 1);
    CGContextFillRect(c, CGRectMake(0, 0, 8, 8));
}
static void pat_release(void *info) { if (info == TAG) n_prel++; }
static void rd_dealloc(void *ref) { if (ref == TAG) n_rddealloc++; }
static CGFloat rd_ascent(void *ref) { n_rdget++; return 12.5; }
static CGFloat rd_descent(void *ref) { n_rdget++; return 3.25; }
static CGFloat rd_width(void *ref) { n_rdget++; return 42.0; }
static const CTRunDelegateCallbacks rd_cb = {kCTRunDelegateVersion1, rd_dealloc, rd_ascent, rd_descent, rd_width};

static unsigned pixel(const uint8_t *px, int w, int x, int y) { const uint8_t *p = px + 4 * (y * w + x); return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void t6(void)
{
    for (int k = 0; k < 256; k++) dp_bytes[k] = (uint8_t)(k * 7);
    CGDataProviderRef dp = CGDataProviderCreateDirect(TAG, 256, &dp_cb_get);
    CFDataRef d = CGDataProviderCopyData(dp);
    unsigned sum = 0;
    for (CFIndex k = 0; k < CFDataGetLength(d); k++) sum = sum * 31 + CFDataGetBytePtr(d)[k];
    printf("data provider (getBytes): length %ld, checksum %08x\n", (long)CFDataGetLength(d), sum);
    CFRelease(d);
    CGDataProviderRelease(dp);
    CGDataProviderDirectCallbacks dpc = {0, dp_ptr, dp_unptr, NULL, dp_release};   /* stack */
    dp = CGDataProviderCreateDirect(TAG, 256, &dpc);
    d = CGDataProviderCopyData(dp);
    printf("data provider (pointer): length %ld, byte 100 = %d\n", (long)CFDataGetLength(d), CFDataGetBytePtr(d)[100]);
    CFRelease(d);
    CGDataProviderRelease(dp);
    printf("data provider: getBytes %s, pointer %s/%s, release %d\n", YN(n_dpget), YN(n_dpptr), YN(n_dpunptr), n_dprel);

    CGDataConsumerCallbacks dcc = {dc_put, dc_release};
    CGDataConsumerRef dc = CGDataConsumerCreate(TAG, &dcc);
    CGRect box = CGRectMake(0, 0, 100, 100);
    CGContextRef pdf = CGPDFContextCreate(dc, &box, NULL);
    CGPDFContextBeginPage(pdf, NULL);
    CGContextFillRect(pdf, CGRectMake(10, 10, 20, 20));
    CGPDFContextEndPage(pdf);
    CGPDFContextClose(pdf);
    CGContextRelease(pdf);
    CGDataConsumerRelease(dc);
    printf("data consumer: putBytes %s, release %d\n", YN(n_put), n_dcrel);

    enum { W = 100, H = 16 };
    static uint8_t px[W * H * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bc = CGBitmapContextCreate(px, W, H, 8, W * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGFloat domain[2] = {0, 1}, range[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    CGFunctionRef fn = CGFunctionCreate(TAG, 1, domain, 4, range, &fn_cb);
    CGShadingRef sh = CGShadingCreateAxial(cs, CGPointMake(0, 0), CGPointMake(W, 0), fn, false, false);
    CGContextDrawShading(bc, sh);
    CGContextFlush(bc);
    printf("function shading: left %08x right %08x, evaluate %s\n", pixel(px, W, 5, 8), pixel(px, W, 95, 8), YN(n_eval));
    CGShadingRelease(sh);
    CGFunctionRelease(fn);
    printf("function: release %d\n", n_frel);

    CGPatternCallbacks pc = {0, pat_draw, pat_release};
    CGPatternRef pat = CGPatternCreate(TAG, CGRectMake(0, 0, 8, 8), CGAffineTransformIdentity, 8, 8, kCGPatternTilingConstantSpacing, true, &pc);
    CGColorSpaceRef pcs = CGColorSpaceCreatePattern(NULL);
    CGContextSetFillColorSpace(bc, pcs);
    CGFloat alpha = 1;
    CGContextSetFillPattern(bc, pat, &alpha);
    CGContextFillRect(bc, CGRectMake(0, 0, W, H));
    CGContextFlush(bc);
    printf("pattern: pixel %08x, draw %s\n", pixel(px, W, 50, 8), YN(n_draw));
    CGContextRelease(bc);
    CGPatternRelease(pat);
    CGColorSpaceRelease(pcs);
    CGColorSpaceRelease(cs);
    printf("pattern: release %d\n", n_prel);

    CTRunDelegateRef rd = CTRunDelegateCreate(&rd_cb, TAG);
    CFStringRef obj = CFSTR("￼");
    CFMutableAttributedStringRef as = CFAttributedStringCreateMutable(NULL, 0);
    CFAttributedStringReplaceString(as, CFRangeMake(0, 0), obj);
    CFAttributedStringSetAttribute(as, CFRangeMake(0, 1), kCTRunDelegateAttributeName, rd);
    CFRelease(rd);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat asc = 0, desc = 0, lead = 0;
    double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
    printf("run delegate: width %.2f ascent %.2f descent %.2f, getters %s\n", w, asc, desc, YN(n_rdget));
    CFRelease(line);
    CFRelease(as);
    printf("run delegate: dealloc %d\n", n_rddealloc);
}

/* ----------------------------------------------------------- 7: AudioUnit */
static int n_render;
static OSStatus au_input(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    if (ref == TAG) n_render++;
    for (UInt32 b = 0; b < io->mNumberBuffers; b++) {
        float *f = io->mBuffers[b].mData;
        for (UInt32 k = 0; k < frames; k++) f[k] = (float)k / frames;
    }
    return noErr;
}
static OSStatus au_beat(void *ud, Float64 *beat, Float64 *tempo) { return noErr; }

static void t7(void)
{
    AudioComponentDescription desc = {kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioUnit au = NULL;
    if (!comp || AudioComponentInstanceNew(comp, &au) || !au) { printf("audio unit: unavailable\n"); return; }
    AudioStreamBasicDescription f = {44100, kAudioFormatLinearPCM,
                                     kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved, 4, 1, 4, 1, 32, 0};
    OSStatus e1 = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    OSStatus e2 = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    AURenderCallbackStruct rc = {au_input, TAG};
    OSStatus e3 = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rc, sizeof rc);
    HostCallbackInfo hc = {TAG, au_beat, NULL, NULL, NULL};
    OSStatus e4 = AudioUnitSetProperty(au, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &hc, sizeof hc);
    OSStatus e5 = AudioUnitInitialize(au);
    enum { N = 512 };
    static float out[N];
    AudioBufferList abl = {1, {{1, N * 4, out}}};
    AudioUnitRenderActionFlags fl = 0;
    AudioTimeStamp ts = {0};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    OSStatus e6 = AudioUnitRender(au, &fl, &ts, 0, N, &abl);
    printf("audio unit: set %d %d %d, host callbacks %s, init %d, render %d, input callback %s, out[128] %.4f out[511] %.4f\n", (int)e1,
           (int)e2, (int)e3, YN(e4 == noErr), (int)e5, (int)e6, YN(n_render), out[128], out[511]);
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
}

/* --------------------------------------------- 8: NSMapTable, NSHashTable */
static int n_mhash, n_meq, n_mret, n_mrel, n_vret, n_vrel, n_hhash, n_heq;
static NSUInteger mk_hash(NSMapTable *t, const void *k) { n_mhash++; return (uintptr_t)k % 10; }
static BOOL mk_equal(NSMapTable *t, const void *a, const void *b) { n_meq++; return (uintptr_t)a % 10 == (uintptr_t)b % 10; }
static void mk_retain(NSMapTable *t, const void *k) { n_mret++; }
static void mk_release(NSMapTable *t, void *k) { n_mrel++; }
static NSString *mk_describe(NSMapTable *t, const void *k) { return @"key"; }
static void mv_retain(NSMapTable *t, const void *v) { n_vret++; }
static void mv_release(NSMapTable *t, void *v) { n_vrel++; }
static NSString *mv_describe(NSMapTable *t, const void *v) { return @"value"; }
static NSUInteger hk_hash(NSHashTable *t, const void *k) { n_hhash++; return (uintptr_t)k % 10; }
static BOOL hk_equal(NSHashTable *t, const void *a, const void *b) { n_heq++; return (uintptr_t)a % 10 == (uintptr_t)b % 10; }
static void hk_retain(NSHashTable *t, const void *k) {}
static void hk_release(NSHashTable *t, void *k) {}
static NSString *hk_describe(NSHashTable *t, const void *k) { return @"elem"; }

static void t8(void)
{
    NSMapTableKeyCallBacks kc = {mk_hash, mk_equal, mk_retain, mk_release, mk_describe, NSNotAPointerMapKey};
    NSMapTableValueCallBacks vc = {mv_retain, mv_release, mv_describe};
    NSMapTable *m = NSCreateMapTable(kc, vc, 0);
    NSMapInsert(m, (void *)11, (void *)100);
    NSMapInsert(m, (void *)21, (void *)200);
    NSMapInsert(m, (void *)3, (void *)300);
    printf("map table: count %lu, get(31) %ld, get(13) %ld\n", (unsigned long)NSCountMapTable(m), (long)NSMapGet(m, (void *)31),
           (long)NSMapGet(m, (void *)13));
    NSFreeMapTable(m);
    printf("map table: hash %s equal %s, key retain=release %s, value retain=release %s\n", YN(n_mhash), YN(n_meq),
           YN(n_mret && n_mret == n_mrel), YN(n_vret && n_vret == n_vrel));
    NSMapTable *mz = NSCreateMapTableWithZone(kc, vc, 0, NULL);
    NSMapInsert(mz, (void *)5, (void *)1);
    printf("map table with zone: get(15) %ld\n", (long)NSMapGet(mz, (void *)15));
    NSFreeMapTable(mz);

    NSHashTableCallBacks hc = {hk_hash, hk_equal, hk_retain, hk_release, hk_describe};
    NSHashTable *h = NSCreateHashTable(hc, 0);
    NSHashInsert(h, (void *)7);
    NSHashInsert(h, (void *)17);
    NSHashInsert(h, (void *)8);
    printf("hash table: count %lu, get(27) %ld, hash %s equal %s\n", (unsigned long)NSCountHashTable(h), (long)NSHashGet(h, (void *)27),
           YN(n_hhash), YN(n_heq));
    NSFreeHashTable(h);
}

/* ------------------------------------------------------- 9: sqlite3 module */
typedef struct { sqlite3_vtab_cursor base; int i; } sv_cursor;
static int n_sv;
static int sv_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **err)
{
    n_sv++;
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(value INTEGER)");
    if (rc) return rc;
    *pp = sqlite3_malloc(sizeof **pp);
    memset(*pp, 0, sizeof **pp);
    return SQLITE_OK;
}
static int sv_disconnect(sqlite3_vtab *v) { n_sv++; sqlite3_free(v); return SQLITE_OK; }
static int sv_bestindex(sqlite3_vtab *v, sqlite3_index_info *ii) { n_sv++; ii->estimatedCost = 10; return SQLITE_OK; }
static int sv_open(sqlite3_vtab *v, sqlite3_vtab_cursor **pc)
{
    sv_cursor *c = sqlite3_malloc(sizeof *c);
    memset(c, 0, sizeof *c);
    *pc = &c->base;
    return SQLITE_OK;
}
static int sv_close(sqlite3_vtab_cursor *c) { sqlite3_free(c); return SQLITE_OK; }
static int sv_filter(sqlite3_vtab_cursor *c, int idx, const char *s, int argc, sqlite3_value **argv) { ((sv_cursor *)c)->i = 1; return SQLITE_OK; }
static int sv_next(sqlite3_vtab_cursor *c) { ((sv_cursor *)c)->i++; return SQLITE_OK; }
static int sv_eof(sqlite3_vtab_cursor *c) { return ((sv_cursor *)c)->i > 10; }
static int sv_column(sqlite3_vtab_cursor *c, sqlite3_context *ctx, int col) { sqlite3_result_int(ctx, ((sv_cursor *)c)->i); return SQLITE_OK; }
static int sv_rowid(sqlite3_vtab_cursor *c, sqlite3_int64 *r) { *r = ((sv_cursor *)c)->i; return SQLITE_OK; }
static const sqlite3_module series_module = {0, sv_connect, sv_connect, sv_bestindex, sv_disconnect, sv_disconnect, sv_open,
                                             sv_close, sv_filter, sv_next, sv_eof, sv_column, sv_rowid};

static void sql_query(sqlite3 *db, const char *q)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL)) { printf("sqlite: prepare: %s\n", sqlite3_errmsg(db)); return; }
    while (sqlite3_step(st) == SQLITE_ROW) printf("sqlite: %s -> %lld %lld\n", q, sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
    sqlite3_finalize(st);
}

static void t9(void)
{
    sqlite3 *db;
    sqlite3_open(":memory:", &db);
    sqlite3_module stack_module = series_module;
    stack_module.iVersion = 1;
    int r1 = sqlite3_create_module_v2(db, "series", &series_module, NULL, NULL);
    int r2 = sqlite3_create_module(db, "series2", &stack_module, NULL);
    int r3 = sqlite3_exec(db, "CREATE VIRTUAL TABLE temp.s USING series; CREATE VIRTUAL TABLE temp.s2 USING series2", NULL, NULL, NULL);
    printf("sqlite: create %d %d %d\n", r1, r2, r3);
    sql_query(db, "SELECT sum(value), count(*) FROM s");
    sql_query(db, "SELECT sum(a.value * b.value), count(*) FROM s a, s2 b WHERE a.value < b.value");
    sqlite3_close(db);
    printf("sqlite: module callbacks %s\n", YN(n_sv));
}

/* ----------------------------------------------- 10: zlib, bzip2, xz allocators */
static int n_zalloc, n_zfree;
static void *z_alloc(void *op, unsigned items, unsigned size) { if (op == TAG) n_zalloc++; return calloc(items, size); }
static void z_free(void *op, void *p) { if (op == TAG) n_zfree++; free(p); }
static void *bz_alloc(void *op, int n, int m) { if (op == TAG) n_zalloc++; return calloc((size_t)n, (size_t)m); }
static void bz_free(void *op, void *p) { if (op == TAG) n_zfree++; free(p); }

typedef struct {
    void *(*alloc)(void *opaque, size_t nmemb, size_t size);
    void (*free)(void *opaque, void *ptr);
    void *opaque;
} xz_allocator;
typedef struct {
    const uint8_t *next_in; size_t avail_in; uint64_t total_in;
    uint8_t *next_out; size_t avail_out; uint64_t total_out;
    const xz_allocator *allocator;
    void *internal, *reserved_ptr[4];
    uint64_t reserved_u64[2];
    size_t reserved_size[2];
    int reserved_enum[2];
} xz_stream;   /* lzma_stream (liblzma base.h); LZMA_STREAM_INIT is all zero */
int lzma_easy_encoder(xz_stream *strm, uint32_t preset, int check);
int lzma_auto_decoder(xz_stream *strm, uint64_t memlimit, uint32_t flags);
int lzma_code(xz_stream *strm, int action);
void lzma_end(xz_stream *strm);
static void *xz_alloc(void *op, size_t n, size_t m) { if (op == TAG) n_zalloc++; return malloc(n * m); }
static void xz_free(void *op, void *p) { if (op == TAG) n_zfree++; free(p); }
static const xz_allocator xz_alloc_const = {xz_alloc, xz_free, &tag};

static unsigned char plain[8192], packed[16384], unpacked[8192];
static unsigned ib_in(void *d, unsigned char **buf) { *buf = packed; return *(unsigned *)d; }
static int ib_out(void *d, unsigned char *buf, unsigned len) { memcpy(unpacked + *(unsigned *)d, buf, len); *(unsigned *)d += len; return 0; }

static void zreport(const char *what, bool ok)
{
    printf("%s: round trip %s, allocator %s, all freed %s\n", what, YN(ok), YN(n_zalloc), YN(n_zalloc == n_zfree));
    n_zalloc = n_zfree = 0;
}

static void t10(void)
{
    for (int k = 0; k < (int)sizeof plain; k++) plain[k] = (unsigned char)("hao callback structs "[k % 21] + (k / 512));
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    zs.zalloc = z_alloc; zs.zfree = z_free; zs.opaque = TAG;
    int r = deflateInit(&zs, 6);
    zs.next_in = plain; zs.avail_in = sizeof plain; zs.next_out = packed; zs.avail_out = sizeof packed;
    r |= deflate(&zs, Z_FINISH) != Z_STREAM_END;
    uLong plen = zs.total_out;
    deflateEnd(&zs);
    z_stream zi;
    memset(&zi, 0, sizeof zi);
    zi.zalloc = z_alloc; zi.zfree = z_free; zi.opaque = TAG;
    r |= inflateInit(&zi);
    zi.next_in = packed; zi.avail_in = (uInt)plen; zi.next_out = unpacked; zi.avail_out = sizeof unpacked;
    r |= inflate(&zi, Z_FINISH) != Z_STREAM_END;
    inflateEnd(&zi);
    zreport("zlib", !r && !memcmp(plain, unpacked, sizeof plain));

    memset(&zs, 0, sizeof zs);
    zs.zalloc = z_alloc; zs.zfree = z_free; zs.opaque = TAG;
    r = deflateInit2(&zs, 9, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = plain; zs.avail_in = sizeof plain; zs.next_out = packed; zs.avail_out = sizeof packed;
    r |= deflate(&zs, Z_FINISH) != Z_STREAM_END;
    plen = zs.total_out;
    deflateEnd(&zs);
    memset(&zi, 0, sizeof zi);
    memset(unpacked, 0, sizeof unpacked);
    zi.zalloc = z_alloc; zi.zfree = z_free; zi.opaque = TAG;
    r |= inflateInit2(&zi, 47);
    zi.next_in = packed; zi.avail_in = (uInt)plen; zi.next_out = unpacked; zi.avail_out = sizeof unpacked;
    r |= inflate(&zi, Z_FINISH) != Z_STREAM_END;
    inflateEnd(&zi);
    zreport("zlib gzip", !r && !memcmp(plain, unpacked, sizeof plain));

    memset(&zs, 0, sizeof zs);
    zs.zalloc = z_alloc; zs.zfree = z_free; zs.opaque = TAG;
    r = deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = plain; zs.avail_in = sizeof plain; zs.next_out = packed; zs.avail_out = sizeof packed;
    r |= deflate(&zs, Z_FINISH) != Z_STREAM_END;
    unsigned in_len = (unsigned)zs.total_out, out_len = 0;
    deflateEnd(&zs);
    static unsigned char window[32768];
    memset(&zi, 0, sizeof zi);
    memset(unpacked, 0, sizeof unpacked);
    zi.zalloc = z_alloc; zi.zfree = z_free; zi.opaque = TAG;
    r |= inflateBackInit(&zi, 15, window);
    r |= inflateBack(&zi, ib_in, &in_len, ib_out, &out_len) != Z_STREAM_END;
    inflateBackEnd(&zi);
    zreport("zlib inflateBack", !r && out_len == sizeof plain && !memcmp(plain, unpacked, sizeof plain));

    bz_stream bs;
    memset(&bs, 0, sizeof bs);
    bs.bzalloc = bz_alloc; bs.bzfree = bz_free; bs.opaque = TAG;
    r = BZ2_bzCompressInit(&bs, 9, 0, 0);
    bs.next_in = (char *)plain; bs.avail_in = sizeof plain; bs.next_out = (char *)packed; bs.avail_out = sizeof packed;
    r |= BZ2_bzCompress(&bs, BZ_FINISH) != BZ_STREAM_END;
    unsigned blen = bs.total_out_lo32;
    BZ2_bzCompressEnd(&bs);
    memset(&bs, 0, sizeof bs);
    memset(unpacked, 0, sizeof unpacked);
    bs.bzalloc = bz_alloc; bs.bzfree = bz_free; bs.opaque = TAG;
    r |= BZ2_bzDecompressInit(&bs, 0, 0);
    bs.next_in = (char *)packed; bs.avail_in = blen; bs.next_out = (char *)unpacked; bs.avail_out = sizeof unpacked;
    r |= BZ2_bzDecompress(&bs) != BZ_STREAM_END;
    BZ2_bzDecompressEnd(&bs);
    zreport("bzip2", !r && !memcmp(plain, unpacked, sizeof plain));

    xz_stream xs;
    memset(&xs, 0, sizeof xs);
    xs.allocator = &xz_alloc_const;
    r = lzma_easy_encoder(&xs, 1, 1 /* LZMA_CHECK_CRC32 */);
    xs.next_in = plain; xs.avail_in = sizeof plain; xs.next_out = packed; xs.avail_out = sizeof packed;
    r |= lzma_code(&xs, 3 /* LZMA_FINISH */) != 1 /* LZMA_STREAM_END */;
    uint64_t xlen = xs.total_out;
    lzma_end(&xs);
    memset(&xs, 0, sizeof xs);
    memset(unpacked, 0, sizeof unpacked);
    xz_allocator xa = xz_alloc_const;   /* stack copy */
    xs.allocator = &xa;
    r |= lzma_auto_decoder(&xs, UINT64_MAX, 0);
    xs.next_in = packed; xs.avail_in = (size_t)xlen; xs.next_out = unpacked; xs.avail_out = sizeof unpacked;
    r |= lzma_code(&xs, 3) != 1;
    lzma_end(&xs);
    zreport("xz", !r && !memcmp(plain, unpacked, sizeof plain));
}

int main(int argc, char **argv)
{
    void (*tests[])(void) = {t1, t2, t3, t4, t5, t6, t7, t8, t9, t10};
    int only = argc > 1 ? atoi(argv[1]) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int k = 0; k < 10; k++) {
        if (only && only != k + 1) continue;
        printf("== %d\n", k + 1);
        tests[k]();
    }
    return 0;
}
