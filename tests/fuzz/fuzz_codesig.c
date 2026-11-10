/*
 * Fuzz target: the embedded code-signature parser in src/guest_sig.c
 * (sig_parse: superblob index, CodeDirectories, best hash rank, sealed
 * special-slot blobs). The signature comes from the guest file, so it is
 * untrusted. The input is the signature blob in an exactly sized buffer;
 * ASan catches any read outside it, UBSan (built in by the Makefile, fatal)
 * any overflow, and a parsed signature must point inside the buffer.
 */
#include "../../src/guest_sig.c"

/* runtime entry points guest_sig.c references; the parser needs none of them */
hao_opts_t hao_opt;
FILE *hao_diag_file;
void hao_diag_prefix(FILE *f) { (void)f; }
bool hao_macho_choose_slice(int fd, uint64_t fsize, uint64_t *off, uint64_t *size, uint32_t *sub, char *why, size_t n)
{ (void)fd; (void)fsize; (void)off; (void)size; (void)sub; (void)why; (void)n; abort(); }
int hao_macho_check_cmds(const uint8_t *cmds, uint32_t sizeofcmds, uint32_t ncmds, int *nsegs, char *why, size_t n)
{ (void)cmds; (void)sizeofcmds; (void)ncmds; (void)nsegs; (void)why; (void)n; abort(); }
bool hao_entitlements_foreign(const char *xml, size_t n) { (void)xml; (void)n; abort(); }
int hao_aot_open_trusted(const hao_aot_dir *d, const char *name, struct stat *st, char *why, size_t n)
{ (void)d; (void)name; (void)st; (void)why; (void)n; abort(); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *b = malloc(size ? size : 1);
    memcpy(b, data, size);
    sig_info s;
    char why[300];
    if (sig_parse(b, size, &s, why, sizeof why)) {
        const uint8_t *e = b + size, *id = (const uint8_t *)s.ident;
        if (s.cd < b || s.h.length > (size_t)(e - s.cd) || id < s.cd || id >= s.cd + s.h.length) abort();
        if (!memchr(id, 0, (size_t)(s.cd + s.h.length - id))) abort();
        if (s.ents && (s.ents < b || s.ents_len > (size_t)(e - s.ents))) abort();
        if (s.code_limit != (s.h.codeLimit64 ? s.h.codeLimit64 : s.h.codeLimit)) abort();
    }
    free(b);
    return 0;
}
