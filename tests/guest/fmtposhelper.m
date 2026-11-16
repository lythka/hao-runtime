#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>

__attribute__((noinline)) NSError *fp_errorv(const char *func, int line, NSString *domain, NSInteger code,
                                             NSError *underlying, NSDictionary *info, NSString *fmt, va_list ap)
{
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    NSMutableDictionary *d = info ? [info mutableCopy] : [NSMutableDictionary dictionary];
    d[NSLocalizedDescriptionKey] = msg;
    d[@"func"] = @(func);
    d[@"line"] = @(line);
    if (underlying) d[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:domain code:code userInfo:d];
}

NSError *fp_error(const char *func, int line, NSString *domain, NSInteger code, NSError *underlying,
                  NSDictionary *info, NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSError *e = fp_errorv(func, line, domain, code, underlying, info, fmt, ap);
    va_end(ap);
    return e;
}

NSString *fp_six(long a, long b, long c, long d, long e, NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    return [NSString stringWithFormat:@"%ld %ld %ld %ld %ld | %@", a, b, c, d, e, s];
}

static char eight[512];
const char *fp_eight(long a, long b, long c, long d, long e, long f, long g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(eight, sizeof eight, "%ld %ld %ld %ld %ld %ld %ld | ", a, b, c, d, e, f, g);
    vsnprintf(eight + n, sizeof eight - (size_t)n, fmt, ap);
    va_end(ap);
    return eight;
}

static char tagged[512];
const char *fp_tagged(const char *fmt, long tag, ...)
{
    va_list ap;
    va_start(ap, tag);
    int n = snprintf(tagged, sizeof tagged, "tag %ld | ", tag);
    vsnprintf(tagged + n, sizeof tagged - (size_t)n, fmt, ap);
    va_end(ap);
    return tagged;
}

static char unopt[512];
__attribute__((optnone)) const char *fp_unopt(long tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(unopt, sizeof unopt, "tag %ld | ", tag);
    vsnprintf(unopt + n, sizeof unopt - (size_t)n, fmt, ap);
    va_end(ap);
    return unopt;
}
