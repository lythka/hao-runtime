#include "varsniffhelper.h"
#include <stdarg.h>
#include <stdio.h>

long vs_sum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int k = 0; k < n; k++) s = s * 3 + va_arg(ap, long);
    va_end(ap);
    return s;
}

int vs_format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vs_scaled(double scale, char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = snprintf(buf, size, "x%g ", scale);
    r += vsnprintf(buf + r, size - (size_t)r, fmt, ap);
    va_end(ap);
    return r;
}

static char five[256];
int vs_five(long a, long b, long c, long d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = snprintf(five, sizeof five, "%ld %ld %ld %ld | ", a, b, c, d);
    r += vsnprintf(five + r, sizeof five - (size_t)r, fmt, ap);
    va_end(ap);
    return r;
}
const char *vs_last_five(void) { return five; }

CFStringRef vs_cf(int level, CFStringRef fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, fmt, ap);
    va_end(ap);
    CFStringRef r = CFStringCreateWithFormat(NULL, NULL, CFSTR("[%d] %@"), level, s);
    CFRelease(s);
    return r;
}

long vs_call(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int k = 0; k < n; k++) {
        long (*f)(long) = va_arg(ap, long (*)(long));
        s = s * 10 + f(k);
    }
    va_end(ap);
    return s;
}
