/*
 * Fuzz target: the embedded code-signature reader, src/libval.c
 * hao_codesig_parse (superblob index, CodeDirectories, best hash rank,
 * sealed special-slot blobs), the one reader hao has. The signature comes
 * from the guest file, so it is untrusted. The input is the signature blob
 * in an exactly sized buffer; ASan catches any read outside it, UBSan
 * (built in by the Makefile, fatal) any overflow, and a parsed signature
 * must point inside the buffer.
 */
#include "../../src/libval.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *b = malloc(size ? size : 1);
    memcpy(b, data, size);
    hao_codesig s;
    char why[300];
    if (hao_codesig_parse(b, size, &s, why, sizeof why)) {
        const uint8_t *e = b + size, *id = (const uint8_t *)s.ident, *team = (const uint8_t *)s.team;
        if (s.cd < b || s.cd_len > (size_t)(e - s.cd) || id < s.cd || id >= s.cd + s.cd_len) abort();
        if (!memchr(id, 0, (size_t)(s.cd + s.cd_len - id))) abort();
        if (team && (team < s.cd || team >= s.cd + s.cd_len || !memchr(team, 0, (size_t)(s.cd + s.cd_len - team)))) abort();
        if (s.ents && (s.ents < b || s.ents_len < 8 || s.ents_len > (size_t)(e - s.ents))) abort();
        if (s.der_ents && (s.der_ents < b || s.der_ents_len < 8 || s.der_ents_len > (size_t)(e - s.der_ents))) abort();
        /* the code slots it names lie in the CodeDirectory (pages_valid indexes them) */
        if (s.hash_off > s.cd_len || (uint64_t)s.n_code_slots * s.hash_size > s.cd_len - s.hash_off) abort();
        if (s.page_shift != 12 && s.page_shift != 14) abort();
        (void)hao_codesig_library_validated(&s);
    }
    free(b);
    return 0;
}
