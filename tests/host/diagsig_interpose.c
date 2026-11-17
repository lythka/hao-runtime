/*
 * Inserted into tests/host/diagsig.c's process (DYLD_INSERT_LIBRARIES): counts
 * calls of the formatting functions and of malloc made while
 * diagsig_in_handler is set, from any image but this one (libhao's included).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int diagsig_in_handler;
unsigned diagsig_fmt_calls, diagsig_alloc_calls;

static void note(unsigned *c)
{
    if (__atomic_load_n(&diagsig_in_handler, __ATOMIC_RELAXED)) __atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
}

static int my_vsnprintf(char *b, size_t n, const char *f, va_list ap)
{
    note(&diagsig_fmt_calls);
    return vsnprintf(b, n, f, ap);
}
static int my_snprintf(char *b, size_t n, const char *f, ...)
{
    note(&diagsig_fmt_calls);
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
extern int __snprintf_chk(char *, size_t, int, size_t, const char *, ...);
extern int __vsnprintf_chk(char *, size_t, int, size_t, const char *, va_list);
static int my_snprintf_chk(char *b, size_t n, int fl, size_t bn, const char *f, ...)
{
    (void)fl; (void)bn;
    note(&diagsig_fmt_calls);
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static int my_vsnprintf_chk(char *b, size_t n, int fl, size_t bn, const char *f, va_list ap)
{
    (void)fl; (void)bn;
    note(&diagsig_fmt_calls);
    return vsnprintf(b, n, f, ap);
}
static int my_vfprintf(FILE *s, const char *f, va_list ap)
{
    note(&diagsig_fmt_calls);
    return vfprintf(s, f, ap);
}
static int my_fprintf(FILE *s, const char *f, ...)
{
    note(&diagsig_fmt_calls);
    va_list ap;
    va_start(ap, f);
    int r = vfprintf(s, f, ap);
    va_end(ap);
    return r;
}
static void *my_malloc(size_t n) { note(&diagsig_alloc_calls); return malloc(n); }
static void *my_calloc(size_t k, size_t n) { note(&diagsig_alloc_calls); return calloc(k, n); }
static void *my_realloc(void *p, size_t n) { note(&diagsig_alloc_calls); return realloc(p, n); }

__attribute__((used)) static struct { const void *mine, *theirs; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)my_snprintf, (const void *)snprintf},
    {(const void *)my_vsnprintf, (const void *)vsnprintf},
    {(const void *)my_snprintf_chk, (const void *)__snprintf_chk},
    {(const void *)my_vsnprintf_chk, (const void *)__vsnprintf_chk},
    {(const void *)my_fprintf, (const void *)fprintf},
    {(const void *)my_vfprintf, (const void *)vfprintf},
    {(const void *)my_malloc, (const void *)malloc},
    {(const void *)my_calloc, (const void *)calloc},
    {(const void *)my_realloc, (const void *)realloc},
};
