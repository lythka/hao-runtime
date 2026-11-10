/* Minimal runtime symbols so jit.o links into the oracle checker. */
#include "hao/runtime.h"
#include <stdio.h>
#include <stdlib.h>
hao_image *hao_image_for_code(uint64_t a) { (void)a; return 0; }
int hao_code_aliased;
const void *hao_code_ptr_slow(uint64_t a) { return (const void *)(uintptr_t)a; }
const void *hao_code_view_slow(uint64_t a, size_t n) { (void)n; return (const void *)(uintptr_t)a; }
bool hao_is_trap(uint64_t pc) { (void)pc; return false; }
int hao_verbose;
void hao_callback_entry(void) {}
int hao_use_jit = 1;
int hao_env_tier(void) { return 0; }
void hao_opts_init(const char *flags) { (void)flags; }
bool hao_bootarg_flag(const char *key) { (void)key; return false; }
FILE *hao_diag_file;
void hao_diag_prefix(FILE *f) { fprintf(f, "hao[oracle]: "); }
void hao_crit_enter(void) {}
void hao_crit_leave(void) {}
int hao_crit_depth(void) { return 1; }
/* jit.o's AOT writer and loader (never reached from the oracle) */
hao_image *hao_map_image_only(const char *path, char *err, size_t errlen) { (void)path; (void)err; (void)errlen; return 0; }
void hao_unload_image(hao_image *img) { (void)img; }
int hao_guest_sig_image(int fd, const char *path, uint64_t slice_off, uint64_t slice_size, uint32_t subtype, uint32_t sig_off,
                        uint32_t sig_size, const hao_range *exec, int nexec, bool signer, const char *team, hao_imgsig *out,
                        char *why, size_t n)
{
    (void)fd; (void)path; (void)slice_off; (void)slice_size; (void)subtype; (void)sig_off; (void)sig_size; (void)exec;
    (void)nexec; (void)signer; (void)team; (void)out; (void)why; (void)n;
    return -1;
}
bool hao_guest_sig_cdhash(const uint8_t *blob, size_t len, uint8_t cdhash[20], uint8_t *hash_type, char *why, size_t n)
{
    (void)blob; (void)len; (void)cdhash; (void)hash_type; (void)why; (void)n;
    return false;
}
