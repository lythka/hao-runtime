/* Mapped Swift calls: src/swiftcall_gen.inc (format 1; its generator is not
 * part of this repository) lists native Swift functions by mangled symbol and
 * the LC_UUID of the x86_64 library that defines them, and for each of the
 * function's values (result, self, error, async context, indirect result,
 * parameters) where each 8-byte word sits on arm64 and on x86_64 and the
 * value's type by its key in the walk's facts table (swiftvalue/Facts.h; 0 a
 * class reference). A guest call of such a function, by its own address, or
 * by the implementation a dispatch thunk's or method descriptor's row names
 * (MethodDescriptor.Impl, the address a vtable call reaches), gets the row on
 * its plan (bridge.c make_plan) and these conversions (runtime.c
 * native_call):
 *   before the call, each argument's words are gathered from the guest's
 *   registers, a SAME value moved as found, any other walked from the
 *   guest's encoding to the native one (swiftvalue.c hao_swift_call_value:
 *   the walk's CONVERT case images and POINTER rule, source guest) and
 *   placed where hao_native_call loads the row's x86_64 register from;
 *   after it, each result's words are gathered from where the native
 *   registers land, walked to the guest's encoding (source native) and
 *   placed in the row's arm64 registers (only the error, when the error
 *   register is nonzero: a function that threw defined no other result).
 * A row the walk cannot apply is refused per the boundary policy
 * (swiftvalue.c), at the call: a REFUSE verdict, a type that does not
 * resolve or whose size is not the row's words, a location hao_native_call
 * does not pass, a value of an indirect result, a coroutine or an async
 * function that needs more than a move. A function without a row keeps the
 * policy for uncertified values.
 *
 * The callback direction: a METHOD_DESCRIPTOR row names a protocol
 * requirement or a class method (...Tq) the other way round when native code
 * calls a guest witness or override of it (swiftmeta.c requirement_bridge,
 * override_bridge; runtime.c callback_plain):
 * before the guest runs, each argument's words are gathered from where the
 * native caller left them (hao_cb_frame), walked to the guest's encoding and
 * placed in the row's arm64 registers; after it, each result's words are
 * gathered from the guest's registers, walked to the native encoding and
 * placed where hao_callback_entry returns the row's x86_64 register from.
 *
 * Under the test switch HAO_SWIFT_WALK_FACTS=FILE (a TEST_SWITCHES build)
 * the fixture file may also hold rows, looked up before the table's:
 *   call SYMBOL UUID KIND VERDICT
 *   value ROLE KEY OWNERSHIP VERDICT WORD...   (the preceding call's; KEY 0 a class reference)
 * KIND GETTER, SETTER, MODIFY, FUNCTION, DISPATCH_THUNK, METHOD_DESCRIPTOR or
 * ASYNC_FUNCTION_POINTER; VERDICT SAME, DYNAMIC, CONVERT or REFUSE; WORD
 * arm64:x86_64 (tests/guest/swiftcallres.sh). */
#include "hao/runtime.h"
#include "hao/bridge.h"
#include "hao/gen_stamp.h"
#include "hao/gmap.h"
#include "hao/opts.h"
#include "hao/swiftvalue.h"
#include "hao/dlsym_once.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { SWIFTCALL_GETTER = 1, SWIFTCALL_SETTER, SWIFTCALL_MODIFY, SWIFTCALL_FUNCTION, SWIFTCALL_DISPATCH_THUNK,
       SWIFTCALL_METHOD_DESCRIPTOR, SWIFTCALL_ASYNC_FUNCTION_POINTER };
#define SWIFTWALK_SAME 'S'
#define SWIFTWALK_DYNAMIC 'D'
#define SWIFTWALK_CONVERT 'C'
#define SWIFTWALK_REFUSE 'R'
static const struct { const char *symbol, *uuid; int kind; char verdict; int first, n; } gen_funcs[] = {
#define SWIFTCALL_FUNCS
#include "swiftcall_gen.inc"
#undef SWIFTCALL_FUNCS
    {NULL, NULL, 0, 0, 0, 0},   /* the end */
};
static const struct { int call; const char *role, *key; char ownership, verdict; const char *words; } gen_values[] = {
#define SWIFTCALL_VALUES
#include "swiftcall_gen.inc"
#undef SWIFTCALL_VALUES
    {-1, NULL, NULL, 0, 0, NULL},
};
#define NFUNCS (sizeof gen_funcs / sizeof *gen_funcs - 1)
#define NVALUES (sizeof gen_values / sizeof *gen_values - 1)

/* A location: a hao_cpu slot, 0-30 x[n], 32-63 the low 8 bytes of v[n - 32]. */
enum { SLOT_NONE = -1, SLOT_V = 32 };
enum { MAX_WORDS = 8, MAX_VALUES = 24 };
typedef struct {
    char role;                 /* r s e a i p (p0, p1, ...: parameters) */
    char verdict;
    const char *key;           /* NULL: a class reference */
    uint8_t n;
    int8_t arm[MAX_WORDS];     /* the arm64 register of each word (a slot) */
    int8_t in[MAX_WORDS];      /* argument: the slot hao_native_call loads the x86_64 register from */
    int8_t out[MAX_WORDS];     /* result: the slot the x86_64 register lands in after the call */
    int8_t cb[MAX_WORDS];      /* callback: the x86_64 register's hao_cb_frame slot (cb_slots) */
    const void *type;          /* resolved metadata (atomic), or (void *)1: does not resolve */
} sc_value;
struct hao_swiftcall {
    const char *symbol;
    char uuid[33];             /* lowercase hex */
    int kind;
    char verdict;
    const char *why;           /* not applied by this runtime: refused at the call */
    const char *cbwhy;         /* not applied to a callback of it (a requirement's row): refused there */
    unsigned nvalues;
    sc_value v[MAX_VALUES];
    uint64_t addr, impl;       /* resolved in its library: the symbol, a descriptor's implementation; */
    bool resolved;             /* both final once this is set (release; read with acquire) */
};
static hao_swiftcall **rows;
static unsigned nrows;
static bool loaded;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hao_gmap libs = HAO_GMAP_INIT(2, 1, 4, 14);   /* mach header -> 1 no rows, 2 rows */
/* A fork that cut off a writer: a load not yet published is redone from the
 * start (its rows are dropped, not freed: the writer may have been in malloc);
 * a resolution not yet published is redone at the next bind (resolve). */
void hao_swiftcall_fork_child(void)
{
    lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) { rows = NULL; nrows = 0; }
}

static int arm_slot(const char *s, size_t n)
{
    char t[8];
    if (n < 2 || n > 3) return SLOT_NONE;
    memcpy(t, s, n); t[n] = 0;
    int k = atoi(t + 1);
    if (t[1] < '0' || t[1] > '9' || (n == 3 && (t[2] < '0' || t[2] > '9'))) return SLOT_NONE;
    if (t[0] == 'x' && (k <= 8 || (k >= 20 && k <= 22))) return k;
    if (t[0] == 'v' && k <= 7) return SLOT_V + k;
    return SLOT_NONE;
}
/* hao_native_call (bridge_x86_64.S): which slot it loads each x86_64 register
 * from, and where it stores each after the call. */
static void x86_slots(const char *s, size_t n, int *in, int *out)
{
    static const struct { const char *name; int8_t in, out; } regs[] = {
        {"rdi", 0, SLOT_NONE}, {"rsi", 1, SLOT_NONE}, {"rdx", 2, 1}, {"rcx", 3, 2}, {"r8", 4, 3}, {"r9", 5, SLOT_NONE},
        {"rax", 8, 0}, {"r13", 20, SLOT_NONE}, {"r12", 21, 21}, {"r14", 22, SLOT_NONE},
    };
    *in = *out = SLOT_NONE;
    for (unsigned k = 0; k < sizeof regs / sizeof *regs; k++)
        if (strlen(regs[k].name) == n && !memcmp(regs[k].name, s, n)) { *in = regs[k].in; *out = regs[k].out; return; }
    if (n == 4 && !memcmp(s, "xmm", 3) && s[3] >= '0' && s[3] <= '7') {
        *in = SLOT_V + (s[3] - '0');
        *out = s[3] <= '3' ? SLOT_V + (s[3] - '0') : SLOT_NONE;
    }
}
static bool is_result(char role) { return role == 'r' || role == 'e'; }
/* hao_cb_frame (hao_callback_entry): an argument register's slot as the
 * native caller left it, or a result register's slot it is returned from;
 * 0-5 gpr[n], 8-11 swift[n - 8], 16-23 xmm[n - 16]. */
enum { CB_SWIFT = 8, CB_XMM = 16 };
static int cb_slot(const char *s, size_t n, bool result)
{
    static const struct { const char *name; int8_t arg, ret; } regs[] = {
        {"rdi", 0, SLOT_NONE}, {"rsi", 1, SLOT_NONE}, {"rdx", 2, 1}, {"rcx", 3, 2}, {"r8", 4, 3}, {"r9", 5, SLOT_NONE},
        {"rax", CB_SWIFT + 3, 0}, {"r12", CB_SWIFT, CB_SWIFT}, {"r13", CB_SWIFT + 1, SLOT_NONE}, {"r14", CB_SWIFT + 2, SLOT_NONE},
    };
    for (unsigned k = 0; k < sizeof regs / sizeof *regs; k++)
        if (strlen(regs[k].name) == n && !memcmp(regs[k].name, s, n)) return result ? regs[k].ret : regs[k].arg;
    if (n == 4 && !memcmp(s, "xmm", 3) && s[3] >= '0' && s[3] <= (result ? '3' : '7')) return CB_XMM + (s[3] - '0');
    return SLOT_NONE;
}

/* One value row into R; false when its text is malformed (the row is then refused). */
static bool add_value(hao_swiftcall *r, const char *role, const char *key, char verdict, const char *words)
{
    if (r->nvalues == MAX_VALUES) { r->why = "more values than this runtime maps"; return false; }
    sc_value *v = &r->v[r->nvalues++];
    memset(v, 0, sizeof *v);
    v->role = role[0];
    v->verdict = verdict;
    v->key = key && strcmp(key, "0") ? key : NULL;
    for (const char *p = words; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p || *p == '\n') break;
        const char *colon = strchr(p, ':'), *end = colon ? colon + strcspn(colon, " \t,\n") : NULL;
        if (!colon || v->n == MAX_WORDS) { r->why = "a value's words are malformed"; return false; }
        int in, out, arm = arm_slot(p, (size_t)(colon - p));
        x86_slots(colon + 1, (size_t)(end - colon - 1), &in, &out);
        v->arm[v->n] = (int8_t)arm; v->in[v->n] = (int8_t)in; v->out[v->n] = (int8_t)out;
        v->cb[v->n] = (int8_t)cb_slot(colon + 1, (size_t)(end - colon - 1), is_result(v->role));
        v->n++;
        if (arm == SLOT_NONE || (is_result(v->role) ? out : in) == SLOT_NONE)
            r->why = "a value word in a location hao_native_call does not pass";
        if (arm == SLOT_NONE || v->cb[v->n - 1] == SLOT_NONE)
            r->cbwhy = "a value word in a location hao_callback_entry does not pass";
        p = end;
    }
    if (!v->n) r->why = "a value without words";
    return true;
}

/* What this runtime cannot apply of a row (its why, NULL: all of it). */
static void settle(hao_swiftcall *r)
{
    bool moves_only = true;
    for (unsigned i = 0; i < r->nvalues; i++) {
        const sc_value *v = &r->v[i];
        if (v->verdict != SWIFTWALK_SAME) moves_only = false;
        if (v->verdict != SWIFTWALK_SAME && v->role == 'i' && !r->why) r->why = "an indirect result's value is not converted by this runtime";
        if (v->verdict != SWIFTWALK_SAME && !v->key && !r->why) r->why = "a class reference that is not SAME";
        if (v->verdict == SWIFTWALK_REFUSE && !r->why) r->why = "a value the call map refuses";
    }
    if (r->verdict == SWIFTWALK_REFUSE && !r->why) r->why = "a call the call map refuses";
    if (!moves_only && !r->why && (r->kind == SWIFTCALL_MODIFY || r->kind == SWIFTCALL_ASYNC_FUNCTION_POINTER))
        r->why = "a coroutine's or async function's values are not converted by this runtime";
    if (r->kind < SWIFTCALL_GETTER || r->kind > SWIFTCALL_ASYNC_FUNCTION_POINTER) r->why = "a call kind this runtime does not know";
}

static hao_swiftcall *new_row(const char *symbol, const char *uuid, int kind, char verdict)
{
    hao_swiftcall *r = calloc(1, sizeof *r);
    hao_swiftcall **g = r ? realloc(rows, (nrows + 1) * sizeof *rows) : NULL;
    if (!g) { free(r); return NULL; }
    rows = g;
    r->symbol = symbol;
    r->kind = kind;
    r->verdict = verdict;
    unsigned o = 0;
    for (const char *p = uuid; *p && o < 32; ++p)
        if (*p != '-') r->uuid[o++] = (char)((*p >= 'A' && *p <= 'F') ? *p - 'A' + 'a' : *p);
    r->uuid[o] = 0;
    if (o != 32) r->why = "a malformed library UUID";
    rows[nrows++] = r;
    return r;
}

static char verdict_of(const char *s)
{
    return !strcmp(s, "SAME") ? SWIFTWALK_SAME : !strcmp(s, "DYNAMIC") ? SWIFTWALK_DYNAMIC :
           !strcmp(s, "CONVERT") ? SWIFTWALK_CONVERT : SWIFTWALK_REFUSE;
}
static int kind_of(const char *s)
{
    static const char *const names[] = {"GETTER", "SETTER", "MODIFY", "FUNCTION", "DISPATCH_THUNK", "METHOD_DESCRIPTOR",
                                        "ASYNC_FUNCTION_POINTER"};
    for (unsigned k = 0; k < sizeof names / sizeof *names; k++) if (!strcmp(s, names[k])) return (int)k + 1;
    return 0;
}

static void load_fixture(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096], a[2048], b[64], c[64], d[16], words[1024], own;
    hao_swiftcall *cur = NULL;
    while (fgets(line, sizeof line, f) && nrows < 256) {
        if (sscanf(line, "call %2047s %63s %63s %15s", a, b, c, d) == 4) {
            cur = new_row(strdup(a), b, kind_of(c), verdict_of(d));
        } else if (cur && sscanf(line, "value %15s %2047s %c %15s %1023[^\n]", c, a, &own, d, words) == 5) {
            (void)own;
            add_value(cur, c, strdup(a), verdict_of(d), strdup(words));
        }
    }
    fclose(f);
}

static void load(void)
{
    if (__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) return;
    hao_crit_enter(); pthread_mutex_lock(&lock);
    if (!loaded) {
        if (hao_test_switches && hao_opt.swift_walk_facts[0]) load_fixture(hao_opt.swift_walk_facts);
        for (unsigned i = 0; i < NFUNCS; i++) {
            hao_swiftcall *r = new_row(gen_funcs[i].symbol, gen_funcs[i].uuid, gen_funcs[i].kind, gen_funcs[i].verdict);
            if (!r) break;
            unsigned have = 0;
            for (int k = gen_funcs[i].first; k >= 0 && k < gen_funcs[i].first + gen_funcs[i].n && (size_t)k < NVALUES; k++)
                if (gen_values[k].call == (int)i) { add_value(r, gen_values[k].role, gen_values[k].key, gen_values[k].verdict, gen_values[k].words); have++; }
            if (have != (unsigned)gen_funcs[i].n && !r->why) r->why = "a call whose value rows are not its own";
        }
        for (unsigned i = 0; i < nrows; i++) settle(rows[i]);
        __atomic_store_n(&loaded, true, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
}

static bool header_uuid(const struct mach_header_64 *h, char out[33])
{
    if (!h || h->magic != MH_MAGIC_64 || h->sizeofcmds > (1u << 24)) return false;
    const uint8_t *p = (const uint8_t *)(h + 1), *end = p + h->sizeofcmds;
    for (uint32_t k = 0; k < h->ncmds && p + sizeof(struct load_command) <= end; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (uint64_t)(end - p)) return false;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            const uint8_t *u = ((const struct uuid_command *)(const void *)lc)->uuid;
            for (unsigned i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", u[i]);
            return true;
        }
        p += lc->cmdsize;
    }
    return false;
}

/* A method descriptor's implementation (MethodDescriptor {Flags, Impl}: a
 * relative direct pointer; 0 none). */
static uint64_t descriptor_impl(const void *d)
{
    int32_t rel;
    if (!d) return 0;
    memcpy(&rel, (const char *)d + 4, 4);
    return rel ? (uint64_t)(uintptr_t)d + 4 + (uint64_t)(int64_t)rel : 0;
}

/* Under lock: R's addresses in its library (HANDLE, at header H). */
static void resolve(hao_swiftcall *r, void *handle, const void *h)
{
    if (__atomic_load_n(&r->resolved, __ATOMIC_ACQUIRE)) return;
    Dl_info di;
    uint64_t addr = 0, impl = 0;
    void *a = dlsym(handle, r->symbol + (r->symbol[0] == '_'));
    if (a && dladdr(a, &di) && di.dli_fbase == h) addr = (uint64_t)(uintptr_t)a;
    size_t n = strlen(r->symbol);
    if (r->kind == SWIFTCALL_METHOD_DESCRIPTOR) impl = descriptor_impl((const void *)(uintptr_t)addr);
    else if (r->kind == SWIFTCALL_DISPATCH_THUNK && n > 2 && n < 1024 && !strcmp(r->symbol + n - 2, "Tj")) {
        char tq[1024];
        memcpy(tq, r->symbol + (r->symbol[0] == '_'), n + 1 - (r->symbol[0] == '_'));
        tq[strlen(tq) - 1] = 'q';
        const void *d = dlsym(handle, tq);
        if (d && dladdr(d, &di) && di.dli_fbase == h) impl = descriptor_impl(d);
    }
    if (impl && (!dladdr((const void *)(uintptr_t)impl, &di) || di.dli_fbase != h)) impl = 0;
    r->addr = addr;
    r->impl = impl;
    __atomic_store_n(&r->resolved, true, __ATOMIC_RELEASE);   /* last: a fork before this redoes it */
}

static const void *value_type(sc_value *v);
const hao_swiftcall *hao_swiftcall_bind(uint64_t native)
{
    if (!native || hao_opt.no_swift_values) return NULL;
    load();
    if (!nrows) return NULL;
    extern const struct mach_header *dyld_image_header_containing_address(const void *addr);   /* libdyld */
    hao_crit_enter();
    const struct mach_header *h = dyld_image_header_containing_address((const void *)(uintptr_t)native);
    hao_crit_leave();
    if (!h) return NULL;
    const uint64_t *e = hao_gmap_get1(&libs, (uint64_t)(uintptr_t)h);
    if (e && e[1] == 1) return NULL;
    char uuid[33];
    bool any = false;
    if (header_uuid((const struct mach_header_64 *)h, uuid))
        for (unsigned i = 0; i < nrows && !any; i++) any = !strcmp(rows[i]->uuid, uuid);
    const hao_swiftcall *found = NULL;
    hao_crit_enter(); pthread_mutex_lock(&lock);
    if (!e) {
        bool fresh;
        uint64_t *s = hao_gmap_claim1(&libs, (uint64_t)(uintptr_t)h, &fresh);
        if (s && fresh) { s[1] = any ? 2 : 1; hao_gmap_publish1(&libs, s, (uint64_t)(uintptr_t)h); }
    }
    if (any) {
        Dl_info di;
        void *handle = dladdr(h, &di) && di.dli_fname ? dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD) : NULL;
        for (unsigned i = 0; i < nrows && !found; i++) {
            hao_swiftcall *r = rows[i];
            if (strcmp(r->uuid, uuid)) continue;
            if (handle) resolve(r, handle, h);
            if (__atomic_load_n(&r->resolved, __ATOMIC_ACQUIRE) && (r->addr == native || r->impl == native)) found = r;
        }
        if (handle) dlclose(handle);
    }
    pthread_mutex_unlock(&lock);
    /* the values' types, now: never first on a call's path (a guest signal handler's call included) */
    for (unsigned i = 0; found && i < found->nvalues; i++)
        if (found->v[i].key && found->v[i].verdict != SWIFTWALK_SAME) (void)value_type((sc_value *)&found->v[i]);
    hao_crit_leave();
    if (found && hao_opt.bridge_check)
        fprintf(HAO_DIAG, "hao: Swift call map: %s at %#llx%s%s\n", found->symbol, (unsigned long long)native,
                found->why ? ": refused, " : "", found->why ? found->why : "");
    return found;
}

const char *hao_swiftcall_symbol(const hao_swiftcall *s) { return s ? s->symbol : NULL; }

/* V's type, resolved once by the native runtime from its key (at binding; under
 * hao_crit, which holds a guest signal back); NULL if it does not resolve. */
static const void *value_type(sc_value *v)
{
    const void *t = __atomic_load_n(&v->type, __ATOMIC_ACQUIRE);
    if (t) return t == (const void *)1 ? NULL : t;
    typedef const void *__attribute__((swiftcall)) by_name_fn(const char *, size_t, const void *, const void *const *);
    by_name_fn *by_name = HAO_DLSYM("swift_getTypeByMangledNameInEnvironment");
    t = by_name && !strstr(v->key, "$yXZ") ? by_name(v->key, strlen(v->key), NULL, NULL) : NULL;
    __atomic_store_n(&v->type, t ? t : (const void *)1, __ATOMIC_RELEASE);
    return t;
}

static uint64_t *slot(hao_cpu *c, int s) { return s >= SLOT_V ? &c->v[s - SLOT_V].d[0] : &c->x[s]; }

/* The values of one direction: arguments before the call (from the guest's
 * registers to where hao_native_call loads the native ones from), results after
 * it (from where the native registers landed to the guest's). */
static void convert(hao_cpu *c, const hao_swiftcall *cs, uint64_t native, bool results)
{
    hao_swiftcall *s = (hao_swiftcall *)cs;
    uint64_t lr = c->x[30];
    if (s->why) {
        if (!results) hao_swift_call_refused(NULL, NULL, s->why, s->symbol, lr, native);
        return;   /* under the audit or HAO_SWIFT_VALUE_PASSTHROUGH: the call as without a row */
    }
    uint64_t before[SLOT_V + 8], buf[MAX_VALUES][MAX_WORDS];
    for (int k = 0; k < SLOT_V + 8; k++) before[k] = *slot(c, k);
    /* a function that threw (its error register nonzero) defined no ordinary result: only the error moves */
    bool threw = false;
    for (unsigned i = 0; results && i < s->nvalues; i++)
        if (s->v[i].role == 'e') threw |= before[s->v[i].out[0]] != 0;
    hao_crit_enter();   /* the walk allocates and takes the Swift runtime's locks: no guest signal on top */
    for (unsigned i = 0; i < s->nvalues; i++) {
        sc_value *v = &s->v[i];
        if (is_result(v->role) != results || (threw && v->role == 'r')) continue;
        for (unsigned w = 0; w < v->n; w++) buf[i][w] = before[results ? v->out[w] : v->arm[w]];
        if (v->verdict == SWIFTWALK_SAME || !v->key) continue;
        const void *type = value_type(v);
        if (!type) { hao_swift_call_refused(NULL, v->key, "the value's type does not resolve", s->symbol, lr, native); continue; }
        hao_swift_call_value(type, buf[i], v->n * 8, results, s->symbol, lr, native);
    }
    hao_crit_leave();
    for (unsigned i = 0; i < s->nvalues; i++) {
        const sc_value *v = &s->v[i];
        if (is_result(v->role) != results || (threw && v->role == 'r')) continue;
        for (unsigned w = 0; w < v->n; w++) *slot(c, results ? v->arm[w] : v->in[w]) = buf[i][w];
    }
}
void hao_swiftcall_before(hao_cpu *c, const hao_swiftcall *s, uint64_t native) { convert(c, s, native, false); }
void hao_swiftcall_after(hao_cpu *c, const hao_swiftcall *s, uint64_t native) { convert(c, s, native, true); }

/* For audit records: the row's symbol of the function at NATIVE, if it has one. */
const char *hao_swiftcall_symbol_at(uint64_t native)
{
    if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) return NULL;
    for (unsigned i = 0; i < nrows; i++)
        if (__atomic_load_n(&rows[i]->resolved, __ATOMIC_ACQUIRE) && (rows[i]->addr == native || rows[i]->impl == native))
            return rows[i]->symbol;
    return NULL;
}

/* The row of the protocol requirement or class method whose descriptor is at
 * REQ (a METHOD_DESCRIPTOR row naming ...Tq), for a native call of a guest
 * witness or override of it; NULL none. At binding (swiftmeta.c), like
 * hao_swiftcall_bind. */
const hao_swiftcall *hao_swiftcall_requirement(uint64_t req)
{
    const hao_swiftcall *r = hao_swiftcall_bind(req);
    return r && r->kind == SWIFTCALL_METHOD_DESCRIPTOR && r->addr == req ? r : NULL;
}

static uint64_t frame_word(const hao_cb_frame *f, int s)
{
    uint64_t w;
    if (s >= CB_XMM) memcpy(&w, f->xmm[s - CB_XMM], 8);
    else w = s >= CB_SWIFT ? f->swift[s - CB_SWIFT] : f->gpr[s];
    return w;
}
static void frame_set(hao_cb_frame *f, int s, uint64_t w)
{
    if (s >= CB_XMM) memcpy(f->xmm[s - CB_XMM], &w, 8);
    else if (s >= CB_SWIFT) f->swift[s - CB_SWIFT] = w;
    else f->gpr[s] = w;
}

/* A native call of a guest witness whose requirement has row CS (runtime.c
 * callback_plain): before the guest runs (RESULTS false), the arguments from
 * the native caller's registers (F) to the row's arm64 registers in C, walked
 * to the guest's encoding; after it, the results from C to F, walked to the
 * native encoding (only the error when the guest threw). */
void hao_swiftcall_callback(hao_cpu *c, hao_cb_frame *f, const hao_swiftcall *cs, bool results)
{
    hao_swiftcall *s = (hao_swiftcall *)cs;
    const char *why = s->why ? s->why : s->cbwhy;
    if (why) {
        if (!results) hao_swift_call_refused(NULL, NULL, why, s->symbol, f->ret, f->target);
        return;
    }
    uint64_t buf[MAX_VALUES][MAX_WORDS];
    bool threw = false;
    for (unsigned i = 0; results && i < s->nvalues; i++)
        if (s->v[i].role == 'e') threw |= *slot(c, s->v[i].arm[0]) != 0;
    hao_crit_enter();
    for (unsigned i = 0; i < s->nvalues; i++) {
        sc_value *v = &s->v[i];
        if (is_result(v->role) != results || (threw && v->role == 'r')) continue;
        for (unsigned w = 0; w < v->n; w++) buf[i][w] = results ? *slot(c, v->arm[w]) : frame_word(f, v->cb[w]);
        if (v->verdict == SWIFTWALK_SAME || !v->key) continue;
        const void *type = value_type(v);
        if (!type) { hao_swift_call_refused(NULL, v->key, "the value's type does not resolve", s->symbol, f->ret, f->target); continue; }
        hao_swift_call_value(type, buf[i], v->n * 8, !results, s->symbol, f->ret, f->target);
    }
    hao_crit_leave();
    for (unsigned i = 0; i < s->nvalues; i++) {
        const sc_value *v = &s->v[i];
        if (is_result(v->role) != results || (threw && v->role == 'r')) continue;
        for (unsigned w = 0; w < v->n; w++) {
            if (results) frame_set(f, v->cb[w], buf[i][w]);
            else *slot(c, v->arm[w]) = buf[i][w];
        }
    }
}
