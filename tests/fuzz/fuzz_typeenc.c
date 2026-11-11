/*
 * Fuzz target: the ObjC type-encoding parser in src/abi.c (encodings come
 * from guest metadata, so they are untrusted). The input, NUL-terminated in
 * an exactly sized buffer, is parsed as a method signature and as a single
 * type. ASan catches any read past the terminator; UBSan (built in by the
 * Makefile, fatal) catches integer overflow in the layout arithmetic; and
 * every parsed size, alignment and HFA count must be in range.
 */
#include "../../src/abi.c"

/* runtime entry points abi.c references; the parser needs none of them */
hao_opts_t hao_opt;
int hao_verbose;
void hao_note_native_block(uint64_t block) { (void)block; }
hao_thread *hao_current(void) { abort(); }
void hao_run(hao_thread *t, const hao_cpu *outer) { (void)t; (void)outer; abort(); }
void hao_sysv_call(uint64_t fn, const uint64_t gpr[6], const uint8_t xmm[8][16], const uint64_t *stack, uint64_t nstack,
                   uint64_t out[4]) { (void)fn; (void)gpr; (void)xmm; (void)stack; (void)nstack; (void)out; abort(); }
uint64_t hao_thunk_for(uint64_t g) { (void)g; abort(); }
uint64_t hao_sig_thunk(uint64_t g, void *s) { (void)g; (void)s; abort(); }
uint64_t hao_sig_thunk_target(uint64_t a) { (void)a; return 0; }
void hao_diag_prefix(FILE *f) { (void)f; }
void hao_log_line(const char *fmt, ...) { (void)fmt; }
void hao_oom(const char *what) { (void)what; abort(); }
FILE *hao_diag_file;
void hao_crit_enter(void) {}
void hao_crit_leave(void) {}

static void check(const htype *t)
{
    if (t->size < 0 || t->size_max < 0 || t->align < 0 || t->align > 16 || t->hfa < 0 || t->hfa > 4) abort();
    if (t->size > t->size_max && t->kind == 's') abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 4096) return 0;
    char *s = malloc(size + 1);
    memcpy(s, data, size);
    s[size] = 0;
    hsig *g = parse_sig(s);
    check(&g->ret);
    if (g->nargs < 0 || g->nargs > MAXARGS) abort();
    for (int k = 0; k < g->nargs; k++) check(&g->args[k]);
    /* a signature the marshalling paths accept has bounded sizes */
    if (!g->unmarshalable) {
        if (g->ret.size_max > MAXSIZE) abort();
        for (int k = 0; k < g->nargs; k++) if (g->args[k].size_max > MAXSIZE) abort();
    }
    free(g);
    htype t;
    hinfo h;
    const char *e = parse_type(s, &t, &h, 0);
    if (e < s || e > s + size || h.nlay < 0 || h.nlay > LAYMAX) abort();
    check(&t);
    free(s);
    return 0;
}
