/* Native stand-ins for tests/guest/aggspi6.c: functions that take a MEMORY-class aggregate by
 * value (SysV: more than 16 bytes, on the stack; AAPCS64: a pointer in a GP register, or an HFA
 * in v-registers) AND fill all six SysV GP argument registers. The plain register bridge then
 * hands the native side the aggregate's pointer as the next scalar and shifts every later
 * argument by (size / 8 - 1) stack slots.
 * Each stand-in has the exported name and the prototype of the 26A428 function it stands for
 * (hao's tables are keyed by name) and prints what it received, so a wrong slot shows.
 *
 * Prototypes, all read from both slices of the 26A428 caches:
 *   SIOpenIndex       SpotlightIndex: both slices' __PRETTY_FUNCTION__ string; SIFileOps
 *                     {^?^?^?} (24 B) from the x86_64 slice's block encoding {SIFileOps=^?^?^?}
 *   SICreateNewIndex  SpotlightIndex: SIOpenIndex minus doc_count_t * (both slices' code, mds+0x72d64)
 *   ess_notify_tcc_modify  libEndpointSecuritySystem: x86 rdi,rsi,edx,ecx, token 0x10..0x2f,
 *                     r8 optional token pointer, r9d, 0x30(%rbp) int; arm64e x0,x1,w2,w3,
 *                     ldp q0,q1,[x4], cbz x5 / ldp q0,q1,[x5], w6, w7
 *   ess_notify_od_group_add  internal ess_notify_od_group_member_change(bool, audit_token_t, int,
 *                     char const*, ess_od_member_type_t, char const*, char const*, char const*)
 *                     (mangled name in the x86_64 slice) minus the bool; x86 edi,rsi,edx,rcx,r8,r9,
 *                     token 0x10..0x2f; arm64e ldp q0,q1,[x0], w1,x2,w3,x4,x5,x6
 *   ess_notify_od_attribute_set  x86 edi,esi,rdx,rcx,r8,r9, token 0x10..0x2f, 0x30/0x38; arm64e
 *                     ldp q0,q1,[x0], w1,w2, x3..x7, [x29,#0x10]
 *   QLPushCustomRoundedRect  QuickLookSupport: x86 rdi, CGRect `leaq 0x10(%rbp)` (32 B), xmm0,
 *                     esi/edx/ecx/r8d tested as bytes, testl r9d, movb 0x30(%rbp); arm64e x0,
 *                     CGRect d0-d3 (HFA), d4, w1-w4, cbz w5, x6; void
 *   CGImageCreateCopyWithParameters  ImageIO: x86 rdi,rsi, 48 B `leaq 0x10(%rbp)` (copied with three
 *                     movups), rdx,rcx,r8,r9, 0x40,0x48 (8 B), 0x50 (int), 0x58 (byte), 0x60 (int);
 *                     arm64e x0,x1, ldp q0,q1,[x2] + ldr q0,[x2,#0x20], x3..x7, [x29,#0x10] (8 B),
 *                     ldr w [+0x18], ldrb [+0x1c], ldr w [+0x20] (packed stack). The 48-byte
 *                     struct's member types are not known; six 8-byte words lay it out alike.
 *   SIUserCtxCreate  SpotlightIndex (P35): x86 rdi, rsi, a 1104-byte struct copied from 0x10(%rbp) with
 *                     `rep movsq` x 0x8a, audit_token_t at 0x460(%rbp), rdx/rcx/r8/r9 handed on to
 *                     __SIUserCtxCreate, 0x480/0x488, 0x490, a byte at 0x498, 0x4a0; arm64e x0, x1,
 *                     memcpy(.., x2, 0x450), `ldp q0, q1, [x3]`, x4..x7, [x29,#0x10] (q: two 8-byte
 *                     words), [x29,#0x20], ldrb [x29,#0x28], [x29,#0x30]; the mds caller fills the
 *                     two from -credential and -auditToken (x8 results). Result: __SIUserCtxCreate's.
 *   NodeCreateSemanticNodeQuery  DesktopServicesPriv (Finder): x86 rdi..r8, the 24-byte record at
 *                     0x10(%rbp); arm64e x0..x4, the record by reference in x5; both pass it on as
 *                     NodeClientContext& (allocate_shared<TClientQuery>), whose retain/release the query
 *                     calls. Result: the query pointer.
 *   MRMediaRemoteRequestVideoThumbnails  MediaRemote: x86 the 2064-byte record at 0x10(%rbp), rdi/rsi/rdx, the
 *                     block in rcx; arm64e the record by reference in x0, x1..x3, the block in x4; the block is
 *                     invoked as block(thumbnails, error). Result: void.
 * The three ess_notify_* return nothing: both slices end in the call to the template
 * `void downcallNotifyOnlyWithAttribution<...>`'s downcallInternal, whose Either<int, Unit> goes to an
 * sret buffer (rdi / x8) that is never read back; eax / w0 at the return are left over.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "aggspi6helper.h"
#define P(x) ((unsigned long long)(uintptr_t)(x))

int SIOpenIndex(void **ref, int fd, void *vp1, void *vp2, uint32_t flags, void *counts,
                SIFileOps ops, void *cbs, void *store, volatile int *cancel)
{
    if (cbs == (void *)0x7777) {   /* call the file operations: mds's are guest functions */
        ops.a(); ops.b(); ops.c();
        printf("open(ops): ref=%llx fd=%d vp1=%llx vp2=%llx flags=%x counts=%llx cbs=%llx store=%llx cancel=%llx\n",
               P(ref), fd, P(vp1), P(vp2), flags, P(counts), P(cbs), P(store), P(cancel));
        return 8;
    }
    if (cbs == (void *)0x5555) {   /* mds's cancel check (__SIOpenIndex+0xd4a: cmpl $0,(cancel)) */
        printf("open(cancel): fd=%d flags=%x ops={%llx,%llx,%llx} store=%llx *cancel=%d\n",
               fd, flags, P(ops.a), P(ops.b), P(ops.c), P(store), *cancel);
        return *cancel ? 0x59 : 0;
    }
    printf("open: ref=%llx fd=%d vp1=%llx vp2=%llx flags=%x counts=%llx ops={%llx,%llx,%llx} cbs=%llx store=%llx cancel=%llx\n",
           P(ref), fd, P(vp1), P(vp2), flags, P(counts), P(ops.a), P(ops.b), P(ops.c), P(cbs), P(store), P(cancel));
    return 7;
}

int SICreateNewIndex(void **ref, int fd, void *vp1, void *vp2, uint32_t flags,
                     SIFileOps ops, void *cbs, void *store, volatile int *cancel)
{
    printf("create: ref=%llx fd=%d vp1=%llx vp2=%llx flags=%x ops={%llx,%llx,%llx} cbs=%llx store=%llx cancel=%llx\n",
           P(ref), fd, P(vp1), P(vp2), flags, P(ops.a), P(ops.b), P(ops.c), P(cbs), P(store), P(cancel));
    return 9;
}

static void tok(const char *what, audit_token_t t)
{
    printf("%s={%x,%x,%x,%x,%x,%x,%x,%x}", what, t.val[0], t.val[1], t.val[2], t.val[3], t.val[4], t.val[5],
           t.val[6], t.val[7]);
}

void ess_notify_tcc_modify(const char *service, const char *identity, int type, int right, audit_token_t token,
                          const audit_token_t *responsible, int reason, int auth)
{
    printf("tcc_modify: service=%s identity=%s type=%d right=%d ", service, identity, type, right);
    tok("token", token);
    if (responsible) tok(" responsible", *responsible); else printf(" responsible=NULL");
    printf(" reason=%d auth=%d\n", reason, auth);
}

void ess_notify_od_group_add(audit_token_t token, int result, const char *group, int member_type,
                            const char *member, const char *node, const char *db)
{
    printf("od_group_add: ");
    tok("token", token);
    printf(" result=%d group=%s member_type=%d member=%s node=%s db=%s\n", result, group, member_type, member,
           node, db);
}

void ess_notify_od_attribute_set(audit_token_t token, int result, int record_type, const char *record,
                                const char *attribute, const char *value0, const char *value1, const char *node,
                                const char *db)
{
    printf("od_attribute_set: ");
    tok("token", token);
    printf(" result=%d record_type=%d record=%s attribute=%s values=%s,%s node=%s db=%s\n", result, record_type,
           record, attribute, value0, value1, node, db);
}

void QLPushCustomRoundedRect(void *ctx, HRect rect, double radius, bool tl, bool tr, bool bl, bool br,
                             int inset, bool close)
{
    printf("rounded_rect: ctx=%llx rect={%g,%g,%g,%g} radius=%g corners=%d%d%d%d inset=%d close=%d\n", P(ctx),
           rect.origin.x, rect.origin.y, rect.size.width, rect.size.height, radius, tl, tr, bl, br, inset, close);
}

void *CGImageCreateCopyWithParameters(void *image, void *space, Params48 p, void *a, void *b, void *c, void *d,
                                      void *e, void *f, int g, bool h, int i)
{
    printf("image_copy: image=%llx space=%llx p={%llx,%llx,%llx,%llx,%llx,%llx} a=%llx b=%llx c=%llx d=%llx"
           " e=%llx f=%llx g=%d h=%d i=%d\n", P(image), P(space), (unsigned long long)p.w[0],
           (unsigned long long)p.w[1], (unsigned long long)p.w[2], (unsigned long long)p.w[3],
           (unsigned long long)p.w[4], (unsigned long long)p.w[5], P(a), P(b), P(c), P(d), P(e), P(f), g, h, i);
    return (void *)(uintptr_t)(0x4000 + (unsigned)i);
}

static uint64_t mix(const uint64_t *p, int n) { uint64_t h = 1469598103934665603ull; for (int i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
void *SIUserCtxCreate(void *alloc, void *a1, SICred1104 cred, audit_token_t tok, void *a4, void *a5, void *a6, void *a7,
                      void *a8, void *a9, void *a10, bool flag, void *a12)
{
    printf("SIUserCtxCreate alloc=%#llx a1=%#llx cred=%016llx first=%#llx,%#llx last=%#llx\n", P(alloc), P(a1),
           (unsigned long long)mix(cred.w, 138), (unsigned long long)cred.w[0], (unsigned long long)cred.w[1],
           (unsigned long long)cred.w[137]);
    printf("  token=%x %x %x %x %x %x %x %x\n", tok.val[0], tok.val[1], tok.val[2], tok.val[3], tok.val[4],
           tok.val[5], tok.val[6], tok.val[7]);
    printf("  a4=%#llx a5=%#llx a6=%#llx a7=%#llx a8=%#llx a9=%#llx a10=%#llx flag=%d a12=%#llx\n", P(a4), P(a5),
           P(a6), P(a7), P(a8), P(a9), P(a10), flag, P(a12));
    return a4;
}

void *NodeCreateSemanticNodeQuery(void *a, void *b, void *c, void *d, void *e, NodeClientContext ctx)
{
    void *held = ctx.retain ? ctx.retain(ctx.info) : ctx.info;
    printf("node-query: a=%llx b=%llx c=%llx d=%llx e=%llx info=%s retained=%s\n", P(a), P(b), P(c), P(d), P(e),
           ctx.info == (void *)0xc0c0 ? "tag" : ctx.info ? "ptr" : "null", held == ctx.info ? "same" : "other");
    if (ctx.release) ctx.release(held);
    return (char *)a + 1;
}

void MRMediaRemoteRequestVideoThumbnails(MRThumbRequest r, void *a, void *b, void *c, void (^done)(void *, void *))
{
    double sum = 0;
    for (int i = 0; i < 256; i++) sum += r.times[i];
    printf("thumbs: sum=%.1f count=%llu size=%.1fx%.1f a=%llx b=%llx c=%llx block=%s\n", sum,
           (unsigned long long)r.count, r.width, r.height, P(a), P(b), P(c), done ? "yes" : "null");
    if (done) done((void *)0x7417, r.count == 3 ? (void *)0 : (void *)0xe44);
}
