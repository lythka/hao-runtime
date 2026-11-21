/* Minimal runtime symbols so jit.o links into the oracle checker. */
#include "hao/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
hao_image *hao_image_for_code(uint64_t a) { (void)a; return 0; }
hao_image *hao_image_for_code_peek(uint64_t a) { (void)a; return 0; }
int hao_code_aliased;
const void *hao_code_ptr_slow(uint64_t a) { return (const void *)(uintptr_t)a; }
const void *hao_code_view_slow(uint64_t a, size_t n) { (void)n; return (const void *)(uintptr_t)a; }
bool hao_is_trap(uint64_t pc) { (void)pc; return false; }
int hao_verbose;
void hao_callback_entry(void) {}
int hao_use_jit = 1;
int hao_env_tier(void) { return 0; }
void hao_opts_init(const char *flags) { (void)flags; }
FILE *hao_diag_file;
void hao_diag_prefix(FILE *f) { fprintf(f, "hao[oracle]: "); }
void hao_oom(const char *what) { fprintf(stderr, "hao[oracle]: %s: out of memory\n", what); abort(); }
void hao_crit_enter(void) {}
void hao_fatal_safe(const char *what, const uint64_t *v, int n)
{
    (void)v; (void)n;
    write(2, what, strlen(what));
    write(2, "\n", 1);
    abort();
}
void hao_crit_leave(void) {}
int hao_crit_depth(void) { return 1; }
/* jit.o's AOT writer and loader (never reached from the oracle) */
hao_image *hao_map_image_only(const char *path, char *err, size_t errlen) { (void)path; (void)err; (void)errlen; return 0; }
void hao_unload_image(hao_image *img) { (void)img; }
hao_slice *hao_slice_open(int fd, char *why, size_t n) { (void)fd; (void)why; (void)n; return 0; }
void hao_slice_close(hao_slice *sl) { (void)sl; }
int hao_slice_verify(hao_slice *sl, bool *bad_exec, char *why, size_t n) { (void)sl; (void)bad_exec; (void)why; (void)n; return -1; }
bool hao_codesig_library_validated(const hao_codesig *cs) { (void)cs; return false; }
bool hao_guest_sig_cdhash(const uint8_t *blob, size_t len, uint8_t cdhash[20], uint8_t *hash_type, char *why, size_t n)
{
    (void)blob; (void)len; (void)cdhash; (void)hash_type; (void)why; (void)n;
    return false;
}

#if defined(HAO_TEST_SWITCHES) || defined(HAO_MAPPED_AOT_TRIAL)
/* The standalone oracle has no guest images, executable cache files or
 * trusted store. Its test-switch build closes the same optional route. */
#include "hao/aot_mapped_vm.h"
bool hao_file_fixed(int fd, const struct stat *st) { return false; }
int hao_libval_mode(void) { return HAO_LIBVAL_OFF; }
bool hao_aot_mapped_open(int fd, size_t size, const uint8_t digest[32], const hao_aot_expect *expect,
                         uint64_t base, hao_aot_mapping *m, char *why, size_t n) { return false; }
void hao_aot_mapping_discard(hao_aot_mapping *m) {}
#endif
