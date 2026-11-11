/*
 * Unit test of the type-encoding parser in src/abi.c: for each type, the
 * size and alignment clang gives it (sizeof/_Alignof), its x86_64 SysV
 * eightbyte classes and its AAPCS64 HFA count must match what hao derives
 * from @encode(type) alone. Built for x86_64 with src/abi.c included.
 *
 * Bitfield types: an encoding does not give a bitfield's declared type.
 * For types declared the way hao assumes (unsigned int, or long long for
 * widths over 32) the assumed layout must be clang's. For any type, clang's
 * layout must be one of the candidates, and when hao calls the type
 * unambiguous, clang's layout must be passed and returned the way the
 * assumed one is.
 */
#include "../../src/abi.c"

/* abi.c's run-time neighbours, not needed for layout checks */
hao_opts_t hao_opt;
void hao_note_native_block(uint64_t block) { (void)block; }
#import <Foundation/Foundation.h>
#include <mach/message.h>
#include <mach-o/fixup-chains.h>
#include <netinet/ip.h>

/* runtime entry points abi.c references; the parser needs none of them */
int hao_verbose;
hao_thread *hao_current(void) { abort(); }
void hao_run(hao_thread *t, const hao_cpu *outer) { (void)outer; abort(); }
void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack, uint64_t out[4]) { abort(); }
uint64_t hao_thunk_for(uint64_t g) { abort(); }
uint64_t hao_sig_thunk(uint64_t g, void *s) { abort(); }
uint64_t hao_sig_thunk_target(uint64_t a) { return 0; }
void hao_diag_prefix(FILE *f) { (void)f; }
void hao_log_line(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
void hao_oom(const char *what) { fprintf(stderr, "%s: out of memory\n", what); abort(); }
FILE *hao_diag_file;
void hao_crit_enter(void) {}
void hao_crit_leave(void) {}

/* classes: "I" INTEGER, "S" SSE, "II", "SI", ...; "M" memory (> 16 bytes)
 * other: declared with bitfield types hao does not assume
 * amb: expected ambiguity, 1 as an argument, 2 as a result */
typedef struct { const char *name, *enc; size_t size, align; const char *cls; int hfa; bool other; int amb; } tcase;
#define T(type, cls, hfa) {#type, @encode(type), sizeof(type), _Alignof(type), cls, hfa, false, 0}
#define TA(type, cls, hfa, amb) {#type, @encode(type), sizeof(type), _Alignof(type), cls, hfa, false, amb}
#define TO(type, cls, hfa, amb) {#type, @encode(type), sizeof(type), _Alignof(type), cls, hfa, true, amb}

struct F1 { float a; };
struct F2 { float a, b; };
struct NST { float a; struct { float b; int c; } s; };          /* SSE (a, b), INT (c) */
struct NST2 { int a; struct { float b; float c; } s; };         /* INT (a, b), SSE (c) */
struct NST3 { float a; struct { float b; } s; double d; };      /* SSE, SSE: HFA? no, mixed */
struct NSTD { struct { double a; } s; struct { double b; } t; }; /* HFA 2 doubles */
union UF { float a; float b[2]; };
union UFI { float a; int b; };
struct SUF { float a; union { float b; float c; } u; };
union UD { double a; double b; };
struct SUD { union { float f[2]; double d; } u; };
struct AF3 { float a[3]; };
struct L3 { long a, b, c; };
struct CI { char c; int i; };
struct DI { double d; int i; };
struct ID { int i; double d; };
struct FFI { float a, b; int c; };
struct FIF { float a; int b; float c; };                           /* INT (a, b), SSE (c) */

/* bitfields */
typedef struct { unsigned a:3, b:5, c:24; } BF32;
typedef struct { unsigned a:8, b:8; float f; } BF3;                 /* 8 bytes, INT */
typedef struct { unsigned a:30; unsigned b:4; float f; } BFF;       /* b moves to bit 32 */
typedef struct { long long a:40; int b:30; } BFK;                   /* b moves to bit 64 */
typedef struct { char c; int :0; char d; } BZ;                      /* d at 4; size 5, align 1 (9 for long long :0) */
typedef struct { float a; int :0; float b; } BHZ;                   /* AAPCS64 ignores the :0: HFA (not for long long :0) */
typedef struct { unsigned a:1; unsigned :7; unsigned b:24; } BUN;
typedef union { struct { unsigned lo:16, hi:16; } p; float f; } BU;
typedef struct { unsigned a:24; unsigned b:9; char c; } BSP;        /* b straddles: moves to bit 32 */
typedef struct { unsigned char a:4, b:4; char c; } BFC;            /* 2 bytes, align 1 */
typedef struct { unsigned char a:4, b:4; char c; short d:12; double x; } BFM;
typedef struct { BFC s; char tail[20]; } BNEST;                     /* 22 bytes; 24 or 32 with wider units */
typedef struct { _Bool a:1, b:1; unsigned short c:9; } BBS;

static const tcase cases[] = {
    T(struct F1, "S", 1), T(struct F2, "S", 2), T(struct NST, "SI", 0), T(struct NST2, "IS", 0),
    T(struct NST3, "SS", 0), T(struct NSTD, "SS", 2), T(union UF, "S", 2), T(union UFI, "I", 0),
    T(struct SUF, "S", 2), T(union UD, "S", 1), T(struct SUD, "S", 0), T(struct AF3, "SS", 3),
    T(struct L3, "M", 0), T(struct CI, "I", 0), T(struct DI, "SI", 0), T(struct ID, "IS", 0),
    T(struct FFI, "SI", 0), T(struct FIF, "IS", 0), T(CGRect, "M", 4), T(CGPoint, "SS", 2),
    T(NSRange, "II", 0),
    T(NSDecimal, "M", 0), T(BF32, "I", 0), T(BF3, "I", 0), T(BFF, "IS", 0), T(BFK, "II", 0), TA(BZ, "I", 0, 3),
    TA(BHZ, "S", 2, 3), T(BUN, "I", 0), T(BU, "I", 0), T(BSP, "I", 0), T(mach_msg_port_descriptor_t, "II", 0),
    T(struct ip, "M", 0),
    TO(BFC, "I", 0, 0), TO(BFM, "IS", 0, 0), TO(BNEST, "M", 0, 1), TO(BBS, "I", 0, 0),
    TO(struct __darwin_fp_control, "I", 0, 0), TO(struct dyld_chained_ptr_64_rebase, "I", 0, 3),
    TO(struct dyld_chained_ptr_arm64e_auth_rebase, "I", 0, 0),
};

static void cls_of(const htype *t, char cls[4])
{
    cls[0] = 0;
    if (t->size > 16) strcpy(cls, "M");
    else for (int e = 0; e < (t->size + 7) / 8; e++) strcat(cls, t->ebc[e] == EB_SSE ? "S" : t->ebc[e] == EB_INT ? "I" : "-");
}

static int check(const tcase *k)
{
    htype t, c; hinfo h;
    parse_type(k->enc, &t, &h, 0);
    /* clang's layout among the candidates */
    int j = 0;
    while (j < h.nlay && !((size_t)h.lay[j].n == k->size && (size_t)h.lay[j].align == k->align)) j++;
    bool found = j < h.nlay;
    c = t;
    if (found) type_from_lay(&c, &h, &h.lay[j]);
    char cls[4], ccls[4];
    cls_of(&t, cls); cls_of(&c, ccls);
    int amb = (t.amb_arg ? 1 : 0) | (t.amb_ret ? 2 : 0);
    bool ok = found && !strcmp(ccls, k->cls) && c.hfa == k->hfa && amb == k->amb;
    if (!k->other) ok &= j == 0;                              /* the assumed layout is clang's */
    if (found && !t.amb_arg) ok &= lay_shape(&h, &h.lay[j], true) == lay_shape(&h, &h.lay[0], true);
    if (found && !t.amb_ret) ok &= lay_shape(&h, &h.lay[j], false) == lay_shape(&h, &h.lay[0], false);
    printf("%-4s %-28.28s %-36.36s size %2d/%-2zu align %d/%zu class %-2s/%-2s hfa %d/%d amb %d/%d candidates %d%s\n",
           ok ? "ok" : "FAIL", k->name, k->enc, t.size, k->size, t.align, k->align, cls, k->cls, t.hfa, k->hfa,
           amb, k->amb, h.nlay, k->other ? " (other types)" : "");
    return ok ? 0 : 1;
}

/* Real method encodings: clang's offsets give each argument's size. */
@interface BFMethods : NSObject
- (void)nest:(BNEST)n tag:(int)t;
- (NSDecimal)dec:(NSDecimal)d k:(int)k;
- (BFC)bfc:(BFC)c d:(BFM)m;
- (void)many:(int)a :(int)b :(int)c :(int)d :(int)e :(int)f :(BNEST)n;
@end
@implementation BFMethods
- (void)nest:(BNEST)n tag:(int)t { }
- (NSDecimal)dec:(NSDecimal)d k:(int)k { return d; }
- (BFC)bfc:(BFC)c d:(BFM)m { return c; }
- (void)many:(int)a :(int)b :(int)c :(int)d :(int)e :(int)f :(BNEST)n { }
@end

static int check_method(SEL sel, int argi, size_t size)
{
    const char *enc = method_getTypeEncoding(class_getInstanceMethod([BFMethods class], sel));
    hsig *s = hao_sig(enc);
    bool ok = s && !s->ambiguous && (size_t)s->args[argi].size == size;
    printf("%-4s method %-40s arg %d size %d/%zu ambiguous %d\n", ok ? "ok" : "FAIL", enc, argi,
           s ? s->args[argi].size : -1, size, s ? s->ambiguous : -1);
    return !ok;
}

/* whole signatures: does the plain register bridge suffice? */
static const struct { const char *enc; bool simple; } sigs[] = {
    {"{?=f{?=fi}}20@0:8i16", false},               /* nested struct: SSE, INT (rax/rdx bridge would be wrong) */
    {"c36@0:8{?=(?={?=(?={?=II}Q)d})}16i32", false}, /* union nested in structs: INT, SSE */
    {"v32@0:8Q16[2f]24", true},                    /* a C array parameter is a pointer */
    {"v64@0:8[16C]16Q24Q32r*40r*48r*56", true},    /* ... so this needs 7 GPRs, not 8 */
    {"{_NSRange=QQ}16@0:8", true},
    {"{CGPoint=dd}16@0:8", true},                  /* HFA of 2 doubles: v0,v1 == xmm0,xmm1 */
    {"(?=f[2f])16@0:8", false},                    /* HFA of 2 floats in v0,v1; x86_64 packs them in xmm0 */
};

/* Malformed or unsizeable encodings: parsed without reading past the
 * terminator (each is parsed from an exact-size heap copy) and refused. */
static const char *const malformed[] = {
    "v16@0:8^", "v16@0:8[3", "{?=i", "v24@0:8[-3d]16", "v24@0:8{?=[1000000000d]}16", "{?=[4]}16@0:8",
    "v16@0:8{?=}8", "{?=b65}16@0:8", "v24@0:8{?=iv}16", "(?=b3", "{?=[2",
    /* element size x count overflowed int before it was clamped (fuzz_typeenc) */
    "v24@0:8{?=[65536[65536i]]}16", "{?=[60000[60000[60000f]]]}16@0:8",
};

static int check_malformed(const char *enc)
{
    char *copy = strdup(enc);
    hsig *s = parse_sig(copy);
    bool ok = s->unmarshalable;
    printf("%-4s malformed %-32.32s unmarshalable %d\n", ok ? "ok" : "FAIL", enc, s->unmarshalable);
    free(copy);
    free(s);
    return !ok;
}

int main(void)
{
    int bad = 0, nsig = sizeof sigs / sizeof *sigs;
    char deep[4096];
    memset(deep, '{', sizeof deep - 1);
    deep[sizeof deep - 1] = 0;
    for (size_t k = 0; k < sizeof malformed / sizeof *malformed; k++) bad += check_malformed(malformed[k]);
    bad += check_malformed(deep);
    nsig += sizeof malformed / sizeof *malformed + 1;
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) bad += check(&cases[k]);
    bad += check_method(@selector(nest:tag:), 2, sizeof(BNEST));
    bad += check_method(@selector(dec:k:), 2, sizeof(NSDecimal));
    bad += check_method(@selector(bfc:d:), 3, sizeof(BFM));
    bad += check_method(@selector(many:::::::), 8, sizeof(BNEST));
    nsig += 4;
    for (int k = 0; k < (int)(sizeof sigs / sizeof *sigs); k++) {
        bool simple = hao_sig_simple(sigs[k].enc), ok = simple == sigs[k].simple;
        printf("%-4s sig %-40s simple %d/%d\n", ok ? "ok" : "FAIL", sigs[k].enc, simple, sigs[k].simple);
        bad += !ok;
    }
    printf("%s: %d of %zu checks differ from clang\n", bad ? "FAIL" : "PASS", bad, sizeof cases / sizeof *cases + nsig);
    return bad != 0;
}
