/* The program a proxy runs (tests/host/reexec.sh): its environment as the
 * guest sees it (environ, getenv, NSProcessInfo) and tagged-pointer objects
 * passed through native Foundation, written to OUTFILE. Must equal the
 * native arm64 run line for line.
 *   child OUTFILE */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
extern char **environ;
int main(int argc, char **argv)
{
    @autoreleasepool {
        FILE *f = argc > 1 ? fopen(argv[1], "w") : stdout;
        if (!f) return 3;
        int n = 0;
        for (char **e = environ; *e; e++, n++)
            if (!strncmp(*e, "OBJC_", 5) || !strncmp(*e, "HAO_SELF", 8) || !strncmp(*e, "HAO_EXEC", 8) || !strncmp(*e, "REEXEC_", 7)) fprintf(f, "environ: %s\n", *e);
        fprintf(f, "environ entries: %d\n", n);
        const char *v = getenv("OBJC_DISABLE_TAG_OBFUSCATION");
        fprintf(f, "getenv OBJC_DISABLE_TAG_OBFUSCATION: %s\n", v ? v : "(unset)");
        NSDictionary *pe = [[NSProcessInfo processInfo] environment];
        fprintf(f, "NSProcessInfo entries: %lu, OBJC_DISABLE_TAG_OBFUSCATION: %s\n", (unsigned long)pe.count,
                pe[@"OBJC_DISABLE_TAG_OBFUSCATION"] ? [pe[@"OBJC_DISABLE_TAG_OBFUSCATION"] UTF8String] : "(unset)");
        fprintf(f, "REEXEC_MARK: %s\n", [pe[@"REEXEC_MARK"] UTF8String] ?: "(unset)");
        /* tagged pointers: small numbers, strings and dates, through native collections and back */
        NSNumber *num = @(42);
        NSString *s = [NSString stringWithFormat:@"%d", 123];
        NSDate *d = [NSDate dateWithTimeIntervalSinceReferenceDate:0];
        NSArray *arr = @[num, s, d, @(-7), @(3.5)];
        NSDictionary *dict = @{s: num, @"k": @(1)};
        fprintf(f, "number %s int %d class %s cf %d\n", num.description.UTF8String, num.intValue,
                class_getName([num class]), CFGetTypeID((__bridge CFTypeRef)num) == CFNumberGetTypeID());
        fprintf(f, "string %s length %lu equal %d class-is-string %d\n", s.UTF8String, (unsigned long)s.length,
                [s isEqualToString:@"123"], [s isKindOfClass:[NSString class]]);
        fprintf(f, "date %.1f\n", d.timeIntervalSinceReferenceDate);
        fprintf(f, "array %s\n", [[arr componentsJoinedByString:@","] UTF8String]);
        fprintf(f, "dict %d %d\n", [dict[@"123"] intValue], [dict[@"k"] intValue]);
        fprintf(f, "sum %ld\n", (long)[[arr subarrayWithRange:NSMakeRange(0, 1)][0] integerValue] + [arr[3] integerValue]);
        if (f != stdout) fclose(f);
    }
    return 0;
}
