#include <ffi/ffi.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

struct pair { double x, y; };

static double sum(struct pair pair)
{
    fputs("entered guest sum\n", stderr);
    return pair.x + pair.y;
}

static void product(ffi_cif *cif, void *result, void **values, void *context)
{
    const struct pair *pair = values[0];
    *(double *)result = pair->x * pair->y + *(double *)context;
    if (cif->abi != FFI_DEFAULT_ABI || cif->nargs != 1) abort();
}

struct graph {
    ffi_type pair;
    ffi_type *members[3], *arguments[1];
    ffi_cif cif;
};

static struct graph *new_graph(void)
{
    struct graph *g = calloc(1, sizeof *g);
    if (!g) abort();
    g->members[0] = g->members[1] = &ffi_type_double;
    g->pair.type = FFI_TYPE_STRUCT;
    g->pair.elements = g->members;
    g->arguments[0] = &g->pair;
    if (ffi_prep_cif(&g->cif, FFI_DEFAULT_ABI, 1, &ffi_type_double, g->arguments)) abort();
    return g;
}

static int variadic_call(void)
{
    ffi_cif cif;
    ffi_type *types[] = {&ffi_type_pointer, &ffi_type_uint64, &ffi_type_pointer, &ffi_type_sint32, &ffi_type_double};
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 3, 5, &ffi_type_sint32, types)) return 1;
    if (cif.abi != FFI_DEFAULT_ABI || cif.nargs != 5 || cif.rtype != &ffi_type_sint32 || cif.arg_types != types) return 1;
    printf("variadic cif bytes=%u flags=%u fixed=%u\n", cif.bytes, cif.flags, cif.aarch64_nfixedargs);
    char out[40], *dest = out;
    size_t cap = sizeof out;
    const char *fmt = "%d %.1f";
    int value = -7;
    double f = 2.5;
    void *args[] = {&dest, &cap, &fmt, &value, &f};
    ffi_arg result = 0;
    ffi_call(&cif, FFI_FN(dlsym(RTLD_DEFAULT, "snprintf")), &result, args);
    return result != 6 || strcmp(out, "-7 2.5");
}

static jmp_buf outer_jump;

static void cross_jump(ffi_cif *cif, void *ret, void **args, void *data)
{
    (void)cif; (void)ret; (void)args; (void)data;
    longjmp(outer_jump, 1);
}

static void nested(ffi_cif *outer, void *ret, void **values, void *context)
{
    (void)outer;
    ffi_call((ffi_cif *)context, FFI_FN(sum), ret, values);
}

int main(int argc, char **argv)
{
    struct graph *a = new_graph();
    ffi_cif rejected;
    if (ffi_prep_cif(&rejected, (ffi_abi)0, 0, &ffi_type_void, NULL) != FFI_BAD_ABI) return 17;
    printf("fixed cif bytes=%u flags=%u fixed=%u\n", a->cif.bytes, a->cif.flags, a->cif.aarch64_nfixedargs);
    struct pair input = {3.5, -2.0};
    void *args[] = {&input};
    double result = 0;
    if (argc == 2 && !strcmp(argv[1], "data-target")) {
        ffi_call(&a->cif, FFI_FN(&input), &result, args);
        return 9;
    }
    ffi_call(&a->cif, FFI_FN(sum), &result, args);
    if (result != 1.5) return 1;
    if (argc == 2 && !strcmp(argv[1], "guest-only")) return 0;
    void *code;
    ffi_closure *closure = ffi_closure_alloc(sizeof *closure, &code);
    double old = 1, current = 2;
    if (!closure) return 2;
    if (argc == 2 && !strcmp(argv[1], "cross-jump")) {
        if (setjmp(outer_jump)) { fputs("escaped native closure\n", stderr); return 15; }
        if (ffi_prep_closure_loc(closure, &a->cif, cross_jump, NULL, code)) return 2;
        ffi_call(&a->cif, FFI_FN(code), &result, args);
        return 16;
    }
    if (ffi_prep_closure_loc(closure, &a->cif, product, &old, code)) return 2;
    struct graph *b = new_graph();
    if (ffi_prep_closure_loc(closure, &b->cif, product, &current, code)) return 3;
    memset(a, 0xa5, sizeof *a);
    free(a);
    ffi_call(&b->cif, FFI_FN(code), &result, args);
    if (result != -5) return 4;
    if (variadic_call()) return 5;
    if (argc == 2 && !strcmp(argv[1], "translated-checks")) {
        if (ffi_prep_closure_loc(closure, &b->cif, product, &old, NULL) == FFI_OK) return 13;
        ffi_cif other;
        if (ffi_prep_cif(&other, FFI_DEFAULT_ABI, 0, &ffi_type_void, NULL)) return 6;
        if (ffi_prep_closure_loc(closure, &other, product, &old, code) == FFI_OK) return 7;
        ffi_type invalid = {.type = FFI_TYPE_STRUCT};
        if (ffi_prep_cif(&other, FFI_DEFAULT_ABI, 0, &invalid, NULL) == FFI_OK) return 8;
        ffi_call(&b->cif, FFI_FN(code), &result, args);
        if (result != -5) return 9;
        /* Model the exact fork snapshot after a writer has copied its
         * prospective guest fields but before publishing the binding. */
        closure->user_data = &old;
        pid_t snapshot = fork();
        if (snapshot == 0) {
            if (closure->user_data != &current) _exit(21);
            ffi_call(&b->cif, FFI_FN(code), &result, args);
            _exit(result == -5 ? 0 : 22);
        }
        int status;
        if (snapshot < 0 || waitpid(snapshot, &status, 0) != snapshot || !WIFEXITED(status) || WEXITSTATUS(status)) return 14;
        closure->user_data = &current;
    }
    if (ffi_prep_closure_loc(closure, &b->cif, nested, &b->cif, code)) return 10;
    pid_t child = fork();
    if (child == 0) {
        ffi_call(&b->cif, FFI_FN(code), &result, args);
        ffi_closure_free(closure);
        _exit(result == 1.5 ? 0 : 1);
    }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 11;
    ffi_call(&b->cif, FFI_FN(code), &result, args);
    if (result != 1.5) return 12;
    ffi_closure_free(closure);
    free(b);
    puts("closure rebind and retired graph passed");
    return 0;
}
