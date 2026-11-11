/* The provenance of hao's generated tables (src/NAME_gen.inc).
 *
 * Every generated table was derived from one macOS build: its arm64e and
 * x86_64 shared caches, its SDK headers and the arm64e files it ships. The
 * generators are not part of this repository; their output is reviewed and
 * committed here, and no row is edited by hand. Each table checks
 * HAO_GEN_FORMAT where it is included, so a table written for another layout
 * does not compile against this runtime; the includer includes this header
 * first.
 *
 * At startup (hao_bridge_init) hao compares HAO_GEN_OS_BUILD with the
 * running system's kern.osversion and, under -v, says when they differ. A
 * different build is expected to work: the tables describe interfaces that
 * rarely change between builds. Where one did, a table can be stale (a
 * function that gained an aggregate parameter, a new variadic export, ...),
 * and the -v line says which build the tables describe. */
#ifndef HAO_GEN_STAMP_H
#define HAO_GEN_STAMP_H

#define HAO_GEN_OS_VERSION "27.0"   /* the macOS release the tables describe */
#define HAO_GEN_OS_BUILD "26A428"   /* its build (kern.osversion) */
#define HAO_GEN_FORMAT 1            /* the table layout the includers read */

/* Under -v: one line when the running system's build is not HAO_GEN_OS_BUILD.
 * Writes the running build to `running` (n bytes; "" when unknown); returns
 * whether it matches. */
#include <stdbool.h>
#include <stddef.h>
bool hao_gen_stamp_check(char *running, size_t n);

#endif
