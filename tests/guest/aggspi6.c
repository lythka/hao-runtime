/* A MEMORY-class aggregate by value in a function that also fills all six SysV GP argument
 * registers (mds `_SIOpenIndex+0xd4a`, really __SIOpenIndex reading its cancel pointer from
 * guest [sp+0x18] = flags << 32). Representative members of the 26A428
 * scale list, called with tagged arguments; each call prints what the callee received:
 *   SIOpenIndex / SICreateNewIndex (mds)  24-byte SIFileOps in the middle, stack args after it
 *   ess_notify_tcc_modify (tccd)          32-byte token after four scalars, more after it
 *   ess_notify_od_group_add (opendirectoryd)  token first, six GP after it
 *   ess_notify_od_attribute_set           token first, eight GP after it (two on both stacks)
 *   QLPushCustomRoundedRect (QuickLookUIHelper)  CGRect: an HFA on arm64, 32 B of x86 stack
 *   CGImageCreateCopyWithParameters (sips)   48 bytes, then packed arm64 stack arguments
 *   NodeCreateSemanticNodeQuery (Finder)  24-byte context after five pointers; the callee calls its
 *                                         retain and release, which are guest functions
 *   MRMediaRemoteRequestVideoThumbnails   a 2064-byte record first, then a guest completion block the callee
 *                                         invokes
 *   CEAcquireUnmanagedContext (derq)      the REAL libCoreEntitlements function (xnu header
 *                                         EntitlementsPriv.h); the setcc gap hid it from P1's scan
 * The first seven are native stand-ins (aggspi6helper.c). Before each call the guest leaves
 * recognisable words (0xdead...) where its outgoing argument area will be. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "aggspi6helper.h"
#define T(x) ((void *)(uintptr_t)(x))

__attribute__((noinline)) static void dirty(void)
{
    volatile uint64_t junk[96];
    for (int i = 0; i < 96; i++) junk[i] = 0xdead000000000000ull | (uint64_t)i;
    (void)junk[0];
}

/* --- SpotlightIndex (mds) --- */
static void fa(void) { puts("guest fileop a"); }
static void fb(void) { puts("guest fileop b"); }
static void fc(void) { puts("guest fileop c"); }
static const SIFileOps kOps = { fa, fb, fc };   /* like mds's __DATA_CONST template 0x1001ebed0 */
__attribute__((noinline)) static int si_open(void)
{
    SIFileOps ops = { (void (*)(void))T(0xa1), (void (*)(void))T(0xa2), (void (*)(void))T(0xa3) };
    return SIOpenIndex(T(0x10), -1, T(0x20), T(0x30), 0x2068, T(0x50), ops, T(0x70), T(0x80), T(0x90));
}
__attribute__((noinline)) static int si_create(void)
{
    SIFileOps ops = { (void (*)(void))T(0xb1), (void (*)(void))T(0xb2), (void (*)(void))T(0xb3) };
    return SICreateNewIndex(T(0x11), -1, T(0x21), T(0x31), 0x24ea, ops, T(0x71), T(0x81), T(0x91));
}
__attribute__((noinline)) static int si_open_cancel(volatile int *cancel)
{   /* the mds fault itself: flags 0x24e8 in the word above the outgoing area */
    SIFileOps ops = { (void (*)(void))T(0xc1), (void (*)(void))T(0xc2), (void (*)(void))T(0xc3) };
    return SIOpenIndex(T(0x14), 5, T(0x24), T(0x34), 0x24e8, T(0x54), ops, T(0x5555), T(0x84), cancel);
}
__attribute__((noinline)) static int si_ops_static(void)
{
    SIFileOps ops = kOps;
    return SIOpenIndex(T(0x12), 3, T(0x22), T(0x32), 1, T(0x52), ops, T(0x7777), T(0x82), T(0x92));
}
__attribute__((noinline)) static int si_ops_runtime(int k)
{
    SIFileOps ops = { k ? fa : fc, fb, k ? fc : fa };
    return SIOpenIndex(T(0x13), 4, T(0x23), T(0x33), 2, T(0x53), ops, T(0x7777), T(0x83), T(0x93));
}

/* --- libEndpointSecuritySystem (tccd, opendirectoryd) --- */
static audit_token_t mktok(unsigned base)
{
    audit_token_t t;
    for (int i = 0; i < 8; i++) t.val[i] = base + (unsigned)i;
    return t;
}
__attribute__((noinline)) static void ess_tcc(int with_responsible)
{
    audit_token_t tk = mktok(0x100), resp = mktok(0x200);
    ess_notify_tcc_modify("kTCCServiceCamera", "com.example.app", 3, 4, tk,
                          with_responsible ? &resp : NULL, 6, 7);
}
__attribute__((noinline)) static void ess_group_add(void)
{
    ess_notify_od_group_add(mktok(0x300), 1, "staff", 2, "alice", "/Local/Default", "db");
}
__attribute__((noinline)) static void ess_attr_set(void)
{
    ess_notify_od_attribute_set(mktok(0x400), 0, 5, "bob", "RealName", "Bob", "B.", "/Local/Default",
                                "db2");
}

/* --- QuickLookSupport (QuickLookUIHelper) --- */
__attribute__((noinline)) static void ql_rect(int k)
{
    HRect r = { { 1.5, 2.5 }, { 30.25, 40.75 } };
    QLPushCustomRoundedRect(T(0xc7c7), r, 4.5, true, false, k & 1, true, k, true);
}

/* --- ImageIO (sips) --- */
__attribute__((noinline)) static void *img_copy(void)
{
    Params48 p = { { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 } };
    return CGImageCreateCopyWithParameters(T(0x1111), T(0x2222), p, T(0xa), T(0xb), T(0xc), T(0xd), T(0xe),
                                           T(0xf), -3, true, 0x77);
}

/* --- libCoreEntitlements (derq): the real function --- */
typedef struct { int64_t version; const uint8_t *blob; const uint8_t *blob_end; } CEValidationResult;
typedef const struct CEError *CEError_t;
extern CEError_t kCENoError, kCEInvalidArgument;
CEError_t CEAcquireUnmanagedContext(const void *rt, CEValidationResult vr, void *ctx);
const char *CEGetErrorString(CEError_t);
static const uint64_t fake_runtime[16];   /* CERuntime_t: only stored, never called on this path */
static void ce_word(const char *what, uint64_t w, const uint8_t *blob, size_t n)
{
    if (w == (uint64_t)(uintptr_t)fake_runtime) printf(" %s=rt", what);
    else if (w >= (uint64_t)(uintptr_t)blob && w <= (uint64_t)(uintptr_t)(blob + n))
        printf(" %s=blob+%llu", what, (unsigned long long)(w - (uint64_t)(uintptr_t)blob));
    else printf(" %s=%llx", what, (unsigned long long)w);
}
__attribute__((noinline)) static void ce_acquire(const char *label, int64_t version, size_t off0, size_t off1,
                                                 int null_ctx)
{
    static const uint8_t blob[64] = { 0x70, 0x1e };   /* DER: tag 0x70, 30 bytes (the v2 path runs der_vm) */
    uint64_t ctx[9];
    memset(ctx, 0x5a, sizeof ctx);
    CEValidationResult vr = { version, blob + off0, blob + off1 };
    CEError_t e = CEAcquireUnmanagedContext(fake_runtime, vr, null_ctx ? NULL : ctx);
    printf("ce %s: %s (%s)", label, CEGetErrorString(e),
           e == kCENoError ? "kCENoError" : e == kCEInvalidArgument ? "kCEInvalidArgument" : "other");
    if (!null_ctx)
        for (int i = 0; i < 9; i++) { char w[8]; snprintf(w, sizeof w, "w%d", i); ce_word(w, ctx[i], blob, 32); }
    printf("\n");
}

/* --- DesktopServicesPriv (Finder) --- */
static int node_refs;
static void *node_retain(void *info) { if (info != T(0xc0c0)) ++*(int *)info; else node_refs++; puts("guest retain"); return info; }
static void node_release(void *info) { if (info != T(0xc0c0)) --*(int *)info; else node_refs--; puts("guest release"); }
__attribute__((noinline)) static void node_query(int counted)
{
    int refs = 1;
    NodeClientContext ctx = { counted ? (void *)&refs : T(0xc0c0), node_retain, node_release };
    void *q = NodeCreateSemanticNodeQuery(T(0x1000), T(0x2000), T(0x3000), T(0x4000), T(0x5000), ctx);
    printf("node-query rc=%p refs=%d node_refs=%d\n", q, refs, node_refs);
    NodeClientContext none = { T(0xc0c0), NULL, NULL };
    printf("node-query-nocb rc=%p\n", NodeCreateSemanticNodeQuery(T(0x1100), T(0x2100), T(0x3100), T(0x4100), T(0x5100), none));
}

/* --- MediaRemote (NowPlayingTouchUI) --- */
__attribute__((noinline)) static void mr_thumbs(int k)
{
    static MRThumbRequest r;
    for (int i = 0; i < 256; i++) r.times[i] = i * 0.5 + k;
    r.count = 3 + (uint64_t)k;
    r.width = 160.0f;
    r.height = 90.0f + (float)k;
    __block int calls = 0;
    MRMediaRemoteRequestVideoThumbnails(r, T(0x31), T(0x32), T(0x33), ^(void *thumbs, void *error) {
        calls++;
        printf("guest block: thumbs=%llx error=%llx k=%d\n", (unsigned long long)(uintptr_t)thumbs,
               (unsigned long long)(uintptr_t)error, k);
    });
    printf("thumbs k=%d calls=%d\n", k, calls);
}

/* one case per argument (the runner runs each case on its own, so a crash in one does not hide
 * the others); no argument runs every case */
static volatile int opaque = 1;
/* --- SpotlightIndex SIUserCtxCreate (mds, P35): two MEMORY aggregates, rdx/r8/r9 handed on --- */
static void si_userctx(int which)
{
    SICred1104 cred;
    audit_token_t tok;
    for (int i = 0; i < 138; i++) cred.w[i] = (which ? 0x200000000ull : 0x100000000ull) * (i < 2 ? i : 1) + (uint64_t)i * 0x101;
    for (int i = 0; i < 8; i++) tok.val[i] = 0x7700u + (unsigned)i + (which ? 0x10u : 0u);
    void *r = SIUserCtxCreate(NULL, T(0x11), cred, tok, T(0x44), T(0x55), T(0x66), T(0x77), T(0x88), T(0x99), T(0xaa),
                              which != 0, which ? T(0xcc) : NULL);
    printf("userctx rc=%p\n", r);
}

static void run_case(const char *c)
{
    volatile int cancel_clear = 0, cancel_set = 1;
    if (!strcmp(c, "si-open")) { dirty(); printf("open rc=%d\n", si_open()); }
    else if (!strcmp(c, "si-create")) { dirty(); printf("create rc=%d\n", si_create()); }
    else if (!strcmp(c, "si-cancel")) {
        dirty(); printf("open-cancel0 rc=%#x\n", si_open_cancel(&cancel_clear));
        dirty(); printf("open-cancel1 rc=%#x\n", si_open_cancel(&cancel_set));
    } else if (!strcmp(c, "si-ops")) {
        dirty(); printf("ops-static rc=%d\n", si_ops_static());
        dirty(); printf("ops-runtime rc=%d\n", si_ops_runtime(opaque));
    } else if (!strcmp(c, "ess-tcc")) {
        dirty(); ess_tcc(1);
        dirty(); ess_tcc(0);
    } else if (!strcmp(c, "ess-group-add")) { dirty(); ess_group_add(); }
    else if (!strcmp(c, "ess-attr-set")) { dirty(); ess_attr_set(); }
    else if (!strcmp(c, "ql-rect")) { dirty(); ql_rect(0); dirty(); ql_rect(opaque); }
    else if (!strcmp(c, "img-copy")) { dirty(); printf("image_copy rc=%p\n", img_copy()); }
    else if (!strcmp(c, "ce")) {
        dirty(); ce_acquire("v2", 2, 0, 32, 0);
        dirty(); ce_acquire("v1", 1, 0, 32, 0);
        dirty(); ce_acquire("null-ctx", 2, 0, 32, 1);
        dirty(); ce_acquire("v0", 0, 0, 32, 0);
    } else if (!strcmp(c, "si-userctx")) { dirty(); si_userctx(0); dirty(); si_userctx(1); }
    else if (!strcmp(c, "mr-thumbs")) { dirty(); mr_thumbs(0); dirty(); mr_thumbs(opaque); }
    else if (!strcmp(c, "node-query")) { dirty(); node_query(0); dirty(); node_query(1); }
    else { printf("unknown case %s\n", c); }
}
static const char *const cases[] = { "si-open", "si-create", "si-cancel", "si-ops", "ess-tcc", "ess-group-add",
                                     "ess-attr-set", "ql-rect", "img-copy", "ce", "si-userctx", "node-query", "mr-thumbs" };
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) for (int i = 1; i < argc; i++) run_case(argv[i]);
    else for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) run_case(cases[i]);
    return 0;
}
