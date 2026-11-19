/*
 * Native callback-call contracts (src/native_callback.c): recipe validation, binding to an image by its
 * UUID, return-address and call-instruction checks, conflicting records, lookups. Fixture records name a
 * call in this test's own image. With a Mach-O file as argument, the shipped records are bound against it
 * as well (a native library extracted from a shared cache: its UUID and call sites) and the count printed.
 */
#include "hao/native_callback.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* An indirect call through a register (callq *%rdx: FF D2) and the label of its return address. */
__asm__(".text\n.globl _nc_caller\n_nc_caller:\n push %rbp\n mov %rsp, %rbp\n callq *%rdx\n"
        ".globl _nc_after_call\n_nc_after_call:\n pop %rbp\n ret\n");
extern char nc_caller[], nc_after_call[];

typedef struct __attribute__((aligned(8))) { uint16_t version, nargs, nrets, stack_bytes; hao_swift_move moves[4]; } recipe4;
#define R(r) ((const hao_swift_reverse *)(const void *)&(r))
static const recipe4 good = {HAO_SWIFT_REVERSE_VERSION, 3, 0, 0,
    {{0, 0, HAO_SR_GP, HAO_SR_GP, 8, 0}, {20, 1, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}, {8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}}};
static const recipe4 other = {HAO_SWIFT_REVERSE_VERSION, 1, 0, 0, {{8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}}};
static const recipe4 bad_version = {HAO_SWIFT_REVERSE_VERSION + 1, 1, 0, 0, {{8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}}};
static const recipe4 bad_special = {HAO_SWIFT_REVERSE_VERSION, 1, 0, 0, {{8, 4, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}}};
static const recipe4 bad_twice = {HAO_SWIFT_REVERSE_VERSION, 2, 0, 0,
    {{8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 8, 0}, {8, 0, HAO_SR_GP, HAO_SR_GP, 8, 0}}};
static const recipe4 bad_x18 = {HAO_SWIFT_REVERSE_VERSION, 1, 0, 0, {{18, 0, HAO_SR_GP, HAO_SR_GP, 8, 0}}};
static const recipe4 bad_size = {HAO_SWIFT_REVERSE_VERSION, 1, 0, 0, {{8, 3, HAO_SR_GP, HAO_SR_SPECIAL, 4, 0}}};
static const recipe4 bad_stack = {HAO_SWIFT_REVERSE_VERSION, 1, 0, 16, {{16, 0, HAO_SR_STACK, HAO_SR_GP, 8, 0}}};

static const struct mach_header_64 *self_header(void)
{
    Dl_info di;
    return dladdr((void *)nc_caller, &di) ? di.dli_fbase : NULL;
}
static void self_uuid(const struct mach_header_64 *h, uint8_t out[16])
{
    const uint8_t *p = (const uint8_t *)(h + 1);
    for (uint32_t k = 0; k < h->ncmds; k++) {
        const struct load_command *lc = (const void *)p;
        if (lc->cmd == LC_UUID) memcpy(out, ((const struct uuid_command *)p)->uuid, 16);
        p += lc->cmdsize;
    }
}

int main(int argc, char **argv)
{
    CHECK(!hao_native_callback_recipe_check(R(good)), "good recipe");
    CHECK(hao_native_callback_recipe_check(R(bad_version)), "recipe version");
    CHECK(hao_native_callback_recipe_check(R(bad_special)), "native special register past rax");
    CHECK(hao_native_callback_recipe_check(R(bad_twice)), "x8 set twice");
    CHECK(hao_native_callback_recipe_check(R(bad_x18)), "x18");
    CHECK(hao_native_callback_recipe_check(R(bad_size)), "special register move of 4 bytes");
    CHECK(hao_native_callback_recipe_check(R(bad_stack)), "guest stack slot past stack_bytes");
    CHECK(hao_native_callback_recipe_check(NULL), "no recipe");
    CHECK(R(good)->nargs == 3 && R(good)->moves[2].arm == 8 && R(good)->moves[2].native == 3, "recipe layout");
    for (unsigned k = 0; k < hao_native_callback_nrows; k++) {
        CHECK(hao_native_callback_rows[k].recipe->nargs + hao_native_callback_rows[k].recipe->nrets > 0, "shipped row %u: no moves", k);
        const char *why = hao_native_callback_recipe_check(hao_native_callback_rows[k].recipe);
        CHECK(!why && hao_native_callback_rows[k].version == HAO_NATIVE_CALLBACK_VERSION, "shipped row %u: %s", k, why ? why : "version");
    }

    const struct mach_header_64 *h = self_header();
    CHECK(h != NULL, "own header");
    if (!h) return 1;
    uint8_t uuid[16] = {0}, wrong[16];
    self_uuid(h, uuid);
    memcpy(wrong, uuid, 16); wrong[0] ^= 1;
    uint64_t ret = (uint64_t)(uintptr_t)nc_after_call - (uint64_t)(uintptr_t)h;
    hao_native_callback_record rows[] = {
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret, R(good)},          /* binds */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret, R(good)},          /* the same again: no conflict */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret, R(other)},         /* conflicts: rejected, the first stays */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret - 1, R(good)},      /* not after a call */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, 1ull << 40, R(good)},   /* outside __TEXT */
        {HAO_NATIVE_CALLBACK_VERSION + 1, {0}, ret, R(good)},      /* record version */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret, R(bad_special)},   /* bad recipe */
        {HAO_NATIVE_CALLBACK_VERSION, {0}, ret, R(good)},          /* another image's UUID: not counted */
    };
    unsigned n = sizeof rows / sizeof rows[0];
    for (unsigned k = 0; k + 1 < n; k++) memcpy(rows[k].uuid, uuid, 16);
    memcpy(rows[n - 1].uuid, wrong, 16);
    hao_gmap map = HAO_GMAP_INIT(2, 1, 4, 16);
    unsigned rejected = 0;
    unsigned bound = hao_native_callback_bind(&map, h, rows, n, &rejected);
    CHECK(bound == 2 && rejected == 5, "bound %u rejected %u (want 2, 5)", bound, rejected);
    CHECK(hao_native_callback_lookup(&map, (uint64_t)(uintptr_t)nc_after_call) == R(good), "lookup at the return address");
    CHECK(!hao_native_callback_lookup(&map, (uint64_t)(uintptr_t)nc_after_call + 1), "lookup one byte past");
    CHECK(!hao_native_callback_lookup(&map, (uint64_t)(uintptr_t)nc_caller), "lookup at the call's function");
    rejected = 0;
    CHECK(hao_native_callback_bind(&map, h, rows + n - 1, 1, &rejected) == 0 && !rejected, "another image's record");
    CHECK(hao_native_callback_bind(&map, NULL, rows, n, NULL) == 0, "no header");

    if (argc > 1) {   /* the shipped records against a native library file */
        int fd = open(argv[1], O_RDONLY);
        struct stat st;
        void *m = fd >= 0 && !fstat(fd, &st) ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        CHECK(m != MAP_FAILED, "map %s", argv[1]);
        if (m != MAP_FAILED) {
            hao_gmap lib = HAO_GMAP_INIT(2, 1, 4, 16);
            rejected = 0;
            bound = hao_native_callback_bind(&lib, m, hao_native_callback_rows, hao_native_callback_nrows, &rejected);
            printf("native callback contracts: %s: %u bound, %u rejected\n", argv[1], bound, rejected);
            CHECK(!rejected, "shipped records rejected against %s", argv[1]);
        }
    }
    printf("native callback contracts: %s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
