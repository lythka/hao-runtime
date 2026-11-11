/*
 * hao — which code stands for which Apple program (docs/security-model.md, Identity).
 *
 * Two root-owned lists in /usr/lib/hao, one format:
 *   stub-identities    tools/hao-trust-list --stubs (hao-image-payload): each embedded Apple
 *                      program's x86_64 stub -> the program's own Apple arm64 slice;
 *   native-identities  the image builder: a native program it re-signed ad hoc -> the
 *                      Apple-signed original it was made from.
 * One row per line ('#' comments):
 *   <observed cdhash, 40 hex>  <reference cdhash, 40 hex>  <arch>  <reference path>
 * observed: the cdhash the kernel reports for the running code. reference: the cdhash of the
 * Apple-signed slice `arch` (arm64e, arm64, x86_64, ...) of the file at `path` (relative to the
 * System root, may contain spaces), whose signature answers for it.
 *
 * Self-contained (no libhao): libhao reads it for the switch tier (trust.c), and libhao_codeid
 * (src/code_identity.c) for Security.framework. A list is read only if root owns it and every
 * directory above it, none of them is writable by group or others, none is a symbolic link and
 * none has an extended ACL; it is read once per process. A row whose reference cdhash is
 * itself an observed cdhash, or equals its own observed cdhash, is dropped (no chains).
 */
#ifndef HAO_IDLIST_H
#define HAO_IDLIST_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t observed[20], reference[20];
    char arch[16];
    char path[1024];              /* absolute */
} hao_idrow;

/* The row for a running or static code's cdhash, or NULL. */
const hao_idrow *hao_idlist_find(const uint8_t cdhash[20]);

#ifdef HAO_TEST_SWITCHES
/* Tests: read the lists from dir instead of /usr/lib/hao, where the owner may be the
 * current user. Call before the first hao_idlist_find. */
void hao_idlist_use_dir_for_test(const char *dir);
#endif
#endif
