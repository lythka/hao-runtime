/* Native helper for tests/guest/varsniff.m: variadic functions that no
 * interposer table names, as private SPI is. Built universal. */
#ifndef VARSNIFFHELPER_H
#define VARSNIFFHELPER_H
#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
long vs_sum(int n, ...);                                               /* n longs */
int vs_format(char *buf, size_t size, const char *fmt, ...);           /* vsnprintf */
int vs_scaled(double scale, char *buf, size_t size, const char *fmt, ...); /* fixed FP argument */
int vs_five(long a, long b, long c, long d, const char *fmt, ...);     /* five fixed GPRs */
CFStringRef vs_cf(int level, CFStringRef fmt, ...);                    /* CFStringCreateWithFormatAndArguments */
long vs_call(int n, ...);                                              /* n functions long (*)(long), each called with its index */
const char *vs_last_five(void);                                        /* what vs_five formatted */
#endif
