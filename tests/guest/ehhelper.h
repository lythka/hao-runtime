/* Native helper for tests/guest/eh.mm (see ehhelper.cpp). */
#ifndef EHHELPER_H
#define EHHELPER_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*eh_fn)(void *arg);
void eh_throw_runtime(const char *msg);           /* throws std::runtime_error */
void eh_throw_int(int v);                         /* throws int */
void eh_call(eh_fn fn, void *arg);                /* calls fn with a native RAII cleanup */
void eh_call_plain(eh_fn fn, void *arg);          /* calls fn, no cleanup */
int eh_catch(eh_fn fn, void *arg, char *buf, size_t len);  /* calls fn inside native try/catch */
int eh_catch_rethrow(eh_fn fn, void *arg);        /* native catch(...) { throw; } around fn */
#ifdef __cplusplus
}
#endif
#endif
