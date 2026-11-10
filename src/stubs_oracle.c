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
