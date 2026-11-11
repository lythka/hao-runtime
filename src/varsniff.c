/*
 * Variadic native functions that no interposer table names.
 *
 * Apple arm64 passes every variadic argument in an 8-byte stack slot; SysV
 * x86_64 passes them like fixed arguments (remaining GPRs, then XMMs, then
 * the stack). The tables in interpose.c cover the public variadics, but the
 * 27.0 arm64-only programs import over a hundred more, mostly private
 * logging and formatting SPI (DBLoggingLogWithFormat, LogPrintF,
 * CPMF_GetLocStrFmt, syslog$DARWIN_EXTSN, ...). Bound directly, the native
 * callee reads its variadic arguments from whatever the guest left in
 * x2..x7 and crashes on the first %@.
 *
 * A SysV variadic function compiled by clang starts with the va_start
 * register save: it stores the GPRs after its fixed ones into the register
 * save area (r9 last, at area+40), then `testb %al,%al; je` over movaps of
 * the XMMs after its fixed floating-point ones. Decoding that prologue gives
 * the number of fixed GPR and FP arguments, so an import whose native
 * definition has it is bound to trap_varsniff instead: the fixed arguments
 * go in registers and the guest's stack slots are passed as SysV variadic
 * arguments. The slots are typed from the format string when the last fixed
 * argument is one in a guest image (a C string or a constant CFString):
 * %f/%e/%g go to XMMs, everything else to GPRs; a format with %Lf is
 * handed over as a copy whose L conversions take a double (vafmt.c). Without a format every slot is an integer, which is exact for the
 * pointer and integer lists those functions take.
 *
 * %a is not one type across the formatters these functions use: printf reads a
 * double, CoreUtils' PrintF (LogPrintF, SNPrintF, ...) a pointer to a hardware or
 * network address (%.6a: six bytes; %##a: a sockaddr). When the format's family is
 * only assumed (no SDK attribute), such a slot goes by its value: an x86_64 user
 * pointer is below 2^47, which as a double is zero or a positive subnormal, and no
 * other double is, so those go to a GPR and the rest to an XMM. Zero is both a NULL
 * address and 0.0: it is an address when the conversion has a '#' flag or a precision,
 * which CoreUtils requires (a bare %a prints an error there), else a double. Left
 * wrong: printf's %#a or %.Na of exactly 0.0, or %a of a positive subnormal, when later
 * slots of the other class follow (they read one slot off).
 *
 * The decoder accepts only the instructions such prologues contain and gives
 * up on anything else, so an unrecognised function stays bound as before.
 */
#include "hao/runtime.h"
#include "hao/gmap.h"
#include "hao/macho.h"
#include "hao/opts.h"
#include "hao/vafmt.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack,
                   uint64_t out[6]);
const char *hao_sigdb_enc(const char *name);
char hao_narrowret_kind_for_name(const char *name);

/* SysV integer argument registers in order, by x86 register number */
static const int argreg[6] = {7 /* rdi */, 6 /* rsi */, 2 /* rdx */, 1 /* rcx */, 8 /* r8 */, 9 /* r9 */};

/* Length of a ModRM operand (with SIB and displacement); base register (or -1 for rip/absolute) and displacement. */
static int modrm_len(const uint8_t *p, int rex, int *base, int32_t *disp)
{
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    *disp = 0;
    *base = rm | (rex & 1) << 3;
    if (mod == 3) return n;
    if (rm == 4) {
        int sib = p[n++];
        *base = (sib & 7) | (rex & 1) << 3;
        if ((sib & 7) == 5 && mod == 0) { *base = -1; memcpy(disp, p + n, 4); return n + 4; }
        if (((sib >> 3) & 7) != 4) *base = -2;     /* indexed: not a save-area store */
    } else if (rm == 5 && mod == 0) {
        *base = -1;
        memcpy(disp, p + n, 4);
        return n + 4;
    }
    if (mod == 1) { *disp = (int8_t)p[n]; n += 1; }
    else if (mod == 2) { memcpy(disp, p + n, 4); n += 4; }
    return n;
}

/* The va_start prologue of a SysV variadic function: fixed GPR and FP argument counts. six: also accept
 * the XMM save with no GPR saved, which a function with six or more fixed integer arguments has, as
 * *ngp = 6 ("six or more": the caller needs another source for the count, objc.c the encoding). */
static bool prologue(const uint8_t *p, int *ngp, int *nfp, bool six)
{
    struct { int reg, base; int32_t disp; } st[24];
    int nst = 0, i = 0;
    for (int insn = 0; insn < 48 && i < 200; insn++) {
        int rex = 0;
        if ((p[i] & 0xf0) == 0x40) rex = p[i++];
        uint8_t op = p[i++];
        int base;
        int32_t disp;
        if (op >= 0x50 && op <= 0x57) continue;                         /* push */
        if (op == 0x90) continue;                                       /* nop */
        if (op == 0x89 || op == 0x8b || op == 0x8d || op == 0x31 || op == 0x33 || op == 0x85 || op == 0x01 ||
            op == 0x03 || op == 0x29 || op == 0x2b || op == 0x39 || op == 0x3b || op == 0x09 || op == 0x0b ||
            op == 0x21 || op == 0x23) {
            int reg = ((p[i] >> 3) & 7) | (rex & 4) << 1;
            bool mem = (p[i] >> 6) != 3;
            i += modrm_len(p + i, rex, &base, &disp);
            if (op == 0x89 && (rex & 8) && mem && nst < 24) st[nst].reg = reg, st[nst].base = base, st[nst++].disp = disp;
            continue;
        }
        if (op == 0x81 || op == 0x83 || op == 0xc7) {
            i += modrm_len(p + i, rex, &base, &disp);
            i += op == 0x83 ? 1 : 4;
            continue;
        }
        if (op >= 0xb8 && op <= 0xbf) { i += (rex & 8) ? 8 : 4; continue; }
        if (op == 0x0f && (p[i] == 0x1f || p[i] == 0x28 || p[i] == 0x29 || p[i] == 0x10 || p[i] == 0x11)) {
            i++;
            i += modrm_len(p + i, rex, &base, &disp);
            continue;
        }
        if (op == 0x84 && p[i] == 0xc0 && !rex) {                        /* testb %al, %al */
            i++;
            if (p[i] == 0x74) i += 2;
            else if (p[i] == 0x0f && p[i + 1] == 0x84) i += 6;
            else return false;
            int first = -1, last = -1;
            for (;;) {                                                  /* movaps or movapd %xmmN, mem (xmm0-7) */
                int o = p[i] == 0x66 ? 1 : 0;
                if (p[i + o] != 0x0f || p[i + o + 1] != 0x29) break;
                int r = (p[i + o + 2] >> 3) & 7;
                if (first < 0) first = r;
                else if (r != last + 1) return false;
                last = r;
                i += o + 2 + modrm_len(p + i + o + 2, 0, &base, &disp);
            }
            if (first < 0 || last != 7) return false;
            /* r9 is saved at area+40; the saved GPRs are the ones after the fixed arguments */
            int k9 = -1;
            for (int k = 0; k < nst; k++) if (st[k].reg == 9 && st[k].base >= 0) k9 = k;
            if (k9 < 0) {
                if (!six) return false;
                *ngp = 6;
                *nfp = first;
                return true;
            }
            int32_t area = st[k9].disp - 40;
            int g = 6;
            for (int a = 5; a >= 0; a--) {
                bool saved = false;
                for (int k = 0; k < nst && !saved; k++)
                    saved = st[k].reg == argreg[a] && st[k].base == st[k9].base && st[k].disp == area + 8 * a;
                if (!saved) break;
                g = a;
            }
            if (g == 0) return false;         /* C has no variadic function without a fixed argument */
            *ngp = g;
            *nfp = first;
            return true;
        }
        return false;                         /* call, jump, ret, pop, anything else */
    }
    return false;
}

bool hao_varsniff_prologue(const uint8_t *p, int *ngp, int *nfp) { return prologue(p, ngp, nfp, false); }

/* The prologue of a native function: only when the address lies in a loaded image's __TEXT
 * (dlsym also returns data and absolute symbols), with the bytes read inside it. */
#define TEXT_CACHE 64
static struct { uint64_t lo, hi; } text_cache[TEXT_CACHE];   /* __TEXT ranges already found; imports cluster by library */
static unsigned text_next;
static pthread_mutex_t text_lock = PTHREAD_MUTEX_INITIALIZER;

static bool native(uint64_t fn, int *ngp, int *nfp, bool six)
{
    if (!fn) return false;
    pthread_mutex_lock(&text_lock);
    bool known = false;
    for (unsigned k = 0; k < TEXT_CACHE && !known; k++) known = fn >= text_cache[k].lo && fn + 256 <= text_cache[k].hi;
    pthread_mutex_unlock(&text_lock);
    if (known) return prologue((const uint8_t *)(uintptr_t)fn, ngp, nfp, six);
    Dl_info d;
    if (!dladdr((const void *)(uintptr_t)fn, &d) || !d.dli_fbase) return false;
    const struct mach_header_64 *mh = d.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return false;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (sg->cmd != LC_SEGMENT_64 || strncmp(sg->segname, "__TEXT", 16)) continue;
        uint64_t lo = (uint64_t)(uintptr_t)mh, hi = lo + sg->vmsize;
        if (!(sg->initprot & 4 /* VM_PROT_EXECUTE */) || fn < lo || fn + 256 > hi) return false;
        pthread_mutex_lock(&text_lock);
        text_cache[text_next].lo = lo, text_cache[text_next].hi = hi;
        text_next = (text_next + 1) % TEXT_CACHE;
        pthread_mutex_unlock(&text_lock);
        return prologue((const uint8_t *)(uintptr_t)fn, ngp, nfp, six);
    }
    return false;
}
bool hao_varsniff_native(uint64_t fn, int *ngp, int *nfp) { return native(fn, ngp, nfp, false); }
bool hao_varsniff_native_imp(uint64_t fn, int *ngp, int *nfp) { return native(fn, ngp, nfp, true); }

/* ---- format typing */

#define VS_SLOTS 256    /* guest stack slots passed at most (a format of more arguments is passed untyped) */
#define VS_MIN 16       /* passed when the format is unknown or shorter */

/* Types of the variadic slots from a format string in a guest image; -1 if `a` is not one.
 * *kind: 'p' a C string, 'C' a constant CFString. hex_open: the family is assumed (hao_fmt_types_chars). */
static int guest_format_types(uint64_t a, char *ty, int max, char *kind, bool hex_open)
{
    hao_image *im = hao_image_for_addr(a);
    if (!im) return -1;
    uint64_t end = hao_image_extent(im, a);
    static uint64_t cfconst;
    if (!cfconst) cfconst = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, "__CFConstantStringClassReference");
    const uint64_t *w = (const uint64_t *)(uintptr_t)a;
    if (cfconst && a + 32 <= end && !(a & 7) && w[0] == cfconst) {
        /* constant CFString: isa, info (0x10 = UTF-16), chars, length */
        bool wide = (w[1] & 0x10) != 0;
        uint64_t chars = w[2], len = w[3];
        hao_image *ci = hao_image_for_addr(chars);
        if (!ci || len > 65536 || chars + len * (wide ? 2 : 1) > hao_image_extent(ci, chars)) return -1;
        *kind = 'C';
        return hao_fmt_types_chars((const void *)(uintptr_t)chars, wide ? 2 : 1, len, ty, max, hex_open);
    }
    const char *s = (const char *)(uintptr_t)a;
    size_t len = 0;
    while (a + len < end && len < 65536 && s[len]) len++;
    if (a + len >= end || len >= 65536) return -1;
    *kind = 'p';
    return hao_fmt_types_chars(s, 1, len, ty, max, hex_open);
}

/* ---- the trap */

/* One record per native function, never freed: its trap's argument. name is
 * the first import bound to it, for -vv trap traces. */
typedef struct { uint64_t fn; uint8_t ngp, nfp; char narrow; const char *name; } vsniff;
static hao_gmap vs = HAO_GMAP_INIT(2, 1, 8, 24);   /* native fn -> vsniff *, under vs_lock */
static pthread_mutex_t vs_lock = PTHREAD_MUTEX_INITIALIZER;

/* Call native variadic fn with the guest's arguments: x0..x(ngp-1) (ngp <= 8) and v0..v(nfp-1) fixed, the guest's
 * stack slots as SysV variadic arguments. fmt: the index of the format among the fixed arguments, or -1;
 * fkind: its family when the SDK declares it ('p', 'C', ...: a format built at run time is typed too),
 * 0 when it is only the last fixed argument by convention (typed when it is a constant in a guest image).
 * Returns whether the slots were typed (from a format, or a scanf format's pointers). */
bool hao_varsniff_call(hao_thread *t, uint64_t fn, int ngp, int nfp, char narrow, int fmt, char fkind)
{
    hao_cpu *c = &t->cpu;
    hao_thunk_native_args(t);           /* guest function pointers and blocks among the fixed arguments */
    uint64_t gpr[6] = {0}, stk[2 * VS_SLOTS + 8], out[6] = {0};   /* two fixed, then x87 values take two each */
    uint8_t xmm[8][16];
    memset(xmm, 0, sizeof xmm);
    /* fixed integer arguments: x0-x7 (at most 8, all in guest registers); SysV takes six in registers
     * and the rest in the first stack slots, ahead of the variadic ones */
    if (ngp > 8) ngp = 8;
    uint64_t fixed[8];
    int gi = ngp, fi = nfp, ns = 0;
    for (int k = 0; k < gi; k++) fixed[k] = c->x[k];
    for (int k = 0; k < fi; k++) memcpy(xmm[k], &c->v[k], 16);
    char ty[VS_SLOTS], kind = 0;
    int nt = -1;
    bool typed = false;
    hao_fmtfix fx = {0};
    bool assumed = !fkind;              /* no SDK attribute: printf's types are a guess (%a above) */
    if (fkind == 'o') fkind = 0;        /* os_log: the logging runtime finds the format by its address */
    if (fmt >= 0 && fmt < gi && (fkind == 's' || fkind == 'W')) {
        fixed[fmt] = hao_fmt_fix(fkind, c->x[fmt], &fx);   /* scanf: every slot is a pointer */
        typed = true;
    } else if (fmt >= 0 && fmt < gi) {
        if (fkind != 'w') nt = guest_format_types(c->x[fmt], ty, VS_SLOTS, &kind, assumed);
        /* %L: Apple arm64 long double is double. The callee gets a copy of the format whose floating
         * conversions take a double (made once per format and kept: the callee may keep the pointer), which
         * is what the arm64 format means, %La included; the slot then goes in an XMM register. */
        bool L = false;
        for (int k = 0; k < nt; k++) L |= ty[k] == 'L';
        if (L) {
            uint64_t f = hao_fmt_fix_const(kind, c->x[fmt]);
            if (f != c->x[fmt]) {
                fixed[fmt] = f;
                for (int k = 0; k < nt; k++) if (ty[k] == 'L') ty[k] = 'd';
            }
        }
        if (nt < 0 && fkind && c->x[fmt]) {   /* the SDK's format, made at run time: fixed per call, then typed */
            fixed[fmt] = hao_fmt_fix(fkind, c->x[fmt], &fx);
            nt = hao_fmt_types(fkind == 'C' ? 'C' : fkind == 'w' ? 'w' : 'p', fixed[fmt], ty, VS_SLOTS);
        }
        typed = nt >= 0;
    }
    for (int k = 0; k < gi; k++) {
        if (k < 6) gpr[k] = fixed[k];
        else stk[ns++] = fixed[k];
    }
    if (gi > 6) gi = 6;
    int nslots = nt > VS_MIN ? nt : VS_MIN;
    const uint64_t *sp = (const uint64_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < nslots; k++) {
        char ty_k = k < nt ? ty[k] : 'i';
        uint64_t s = sp[k];
        if (ty_k == 'a' || ty_k == 'x')                /* sign and exponent 0: a pointer, or a zero or subnormal double */
            ty_k = s >> 52 || (!s && ty_k == 'x') ? 'd' : 'i';
        if (ty_k == 'd') {
            if (fi < 8) memcpy(xmm[fi++], &s, 8);
            else stk[ns++] = s;
        } else if (ty_k == 'L') {       /* no copy of the format: the x87 value, in memory and 16-aligned (SysV) */
            double d;
            long double ld;
            memcpy(&d, &s, 8);
            ld = d;
            ns += ns & 1;
            memset(&stk[ns], 0, 16);
            memcpy(&stk[ns], &ld, 10);
            ns += 2;
        } else {
            if (hao_is_guest_function(s)) s = hao_thunk_for(s);
            if (gi < 6) gpr[gi++] = s;
            else stk[ns++] = s;
        }
    }
    hao_sysv_call(fn, gpr, (const uint8_t (*)[16])xmm, stk, (uint64_t)ns, out);
    hao_fmt_done(&fx);
    c->x[0] = narrow ? hao_narrowret_extend(out[0], narrow) : out[0];
    c->x[1] = out[1];
    memset(&c->v[0], 0, sizeof c->v[0]);
    memcpy(&c->v[0], &out[2], 8);
    return typed;
}

static void trap_varsniff(hao_thread *t, uint64_t rec)
{
    const vsniff *v = (const vsniff *)(uintptr_t)rec;
    hao_varsniff_call(t, v->fn, v->ngp, v->nfp, v->narrow, v->ngp - 1, 0);
}

/* ---- native ObjC method IMPs (objc.c trap_msgsend): the prologue verdict per IMP
 *
 * objc_msgSend reaches native methods by IMP, not by an import, so there is no bind time: the first
 * send of each native IMP decodes its prologue and the verdict is kept. Every native send looks it up,
 * so the table is read without a lock (open addressing on one word per IMP, grown by replacement as
 * the plan table is; the old tables stay valid for readers). Inserts run in a hao critical section
 * under vm_lock. An entry is the IMP (x86_64 user addresses fit in 48 bits) with the verdict above it. */
#define VM_IMP 0x0000ffffffffffffull
#define VM_HAS (1ull << 63)
#define VM_VAR (1ull << 62)
#define VM_NOTE_SHIFT 56                   /* one-time report bits (hao_varsniff_imp_note) */
typedef struct vm_tab { uint32_t mask, used; struct vm_tab *prev; uint64_t s[]; } vm_tab;
static vm_tab *vm_cur;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t vm_hash(uint64_t a, uint32_t mask) { return (uint32_t)((a * 0x9E3779B97F4A7C15ull) >> 32) & mask; }
static uint64_t *vm_slot(vm_tab *t, uint64_t imp)
{
    for (uint32_t h = vm_hash(imp, t->mask);; h = (h + 1) & t->mask) {
        uint64_t e = __atomic_load_n(&t->s[h], __ATOMIC_ACQUIRE);
        if (!e || (e & VM_IMP) == imp) return &t->s[h];
    }
}
static vm_tab *vm_new(uint32_t n)
{
    void *m = mmap(NULL, sizeof(vm_tab) + (size_t)n * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return NULL;
    vm_tab *t = m;
    t->mask = n - 1;
    return t;
}

/* *ngp, *nfp: the fixed GPR and FP arguments when imp is a SysV variadic function. *fresh: this call
 * decoded it (the caller reports a new variadic IMP once). decode false (a guest signal handler runs:
 * dladdr could wait for a lock the interrupted code holds): an IMP not seen before is taken as not
 * variadic, as bind_bare (bridge.c) leaves such addresses unbound. */
bool hao_varsniff_imp(uint64_t imp, bool decode, int *ngp, int *nfp, bool *fresh)
{
    *fresh = false;
    if (!imp) return false;
    uint64_t e = 0;
    vm_tab *t = __atomic_load_n(&vm_cur, __ATOMIC_ACQUIRE);
    if (t && !(imp & ~VM_IMP)) e = *vm_slot(t, imp);
    if (!e && !decode) return false;
    if (!e) {
        int g = 0, f = 0;
        hao_crit_enter();                  /* text_lock, dladdr's dyld lock and vm_lock: no guest signal handler meanwhile */
        bool v = hao_varsniff_native_imp(imp, &g, &f);
        e = VM_HAS | imp | (v ? VM_VAR | (uint64_t)g << 52 | (uint64_t)f << 48 : 0);
        if (!(imp & ~VM_IMP)) {
            pthread_mutex_lock(&vm_lock);
            t = vm_cur;
            if (!t || (t->used + 1) * 4 > (t->mask + 1) * 3) {
                vm_tab *n = vm_new(t ? 2 * (t->mask + 1) : 1024);
                if (n) {
                    for (uint32_t k = 0; t && k <= t->mask; k++)
                        if (t->s[k]) { *vm_slot(n, t->s[k] & VM_IMP) = t->s[k]; n->used++; }
                    n->prev = t;
                    __atomic_store_n(&vm_cur, n, __ATOMIC_RELEASE);
                    t = n;
                }
            }
            if (t && (t->used + 1) * 4 <= (t->mask + 1) * 3) {
                uint64_t *s = vm_slot(t, imp);
                if (!*s) { __atomic_store_n(s, e, __ATOMIC_RELEASE); t->used++; *fresh = v; }
                else e = *s;
            } else *fresh = v;             /* no memory: decoded again next time */
            pthread_mutex_unlock(&vm_lock);
        } else *fresh = v;
        hao_crit_leave();
    }
    if (!(e & VM_VAR)) return false;
    *ngp = (int)(e >> 52 & 15);
    *nfp = (int)(e >> 48 & 15);
    return true;
}

/* A one-time report for imp (bit 0-5): true the first time it is asked for. */
bool hao_varsniff_imp_note(uint64_t imp, unsigned bit)
{
    vm_tab *t = __atomic_load_n(&vm_cur, __ATOMIC_ACQUIRE);
    if (!t || (imp & ~VM_IMP) || bit > 5) return true;
    uint64_t *s = vm_slot(t, imp);
    if (!__atomic_load_n(s, __ATOMIC_ACQUIRE)) return true;
    uint64_t b = 1ull << (VM_NOTE_SHIFT + bit);
    return !(__atomic_fetch_or(s, b, __ATOMIC_RELAXED) & b);
}

void hao_varsniff_fork_child(void)
{
    text_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    vs_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    vm_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

/* Bind `name` (native definition at `native`) to the trap when its native code is a
 * variadic function no table handles. Called by the import resolver after the tables. */
bool hao_varsniff_bind(const char *name, uint64_t native, uint64_t *addr)
{
    if (hao_opt.no_varsniff || !native) return false;
    if (hao_sigdb_enc(name) || hao_stackargs_enc(name)) return false;   /* aggregates: the signature path */
    int ngp, nfp;
    if (!hao_varsniff_native(native, &ngp, &nfp)) return false;
    pthread_mutex_lock(&vs_lock);
    bool fresh;
    uint64_t *s = hao_gmap_claim1(&vs, native, &fresh);
    /* an import left out would bind straight to the native variadic function */
    if (!s) hao_oom("native variadic functions");
    vsniff *v;
    if (fresh) {
        v = hao_alloc_check(malloc(sizeof *v), "native variadic functions");
        *v = (vsniff){native, (uint8_t)ngp, (uint8_t)nfp, hao_narrowret_kind_for_name(name),
                      hao_alloc_check(strdup(name), "native variadic functions")};
        s[1] = (uint64_t)(uintptr_t)v;
        hao_gmap_publish1(&vs, s, native);
    } else v = (vsniff *)(uintptr_t)s[1];
    pthread_mutex_unlock(&vs_lock);
    HAO_LOG(1, "%s: native variadic (%d fixed, %d fixed FP)\n", name, ngp, nfp);
    *addr = hao_trap_address(v->name, trap_varsniff, (uint64_t)(uintptr_t)v);
    return true;
}
