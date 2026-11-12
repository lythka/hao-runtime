/* NSUnarchiver decoding with arm64's @encode(BOOL) ("B") of archives that hold
 * a 'c' (BOOL as x86_64 and older archives write it) or a 'B'. arm64
 * Foundation accepts an archived 'c' where "B" is asked for; x86_64's throws
 * NSArchiverArchiveInconsistency. tests/guest/unarchbool.sh. */
#import <Foundation/Foundation.h>

typedef struct { BOOL flag; int n; } Pair;
typedef struct { char c; int n; } PairC;
typedef struct Bx { BOOL b; } Bx;    /* a struct whose name holds a 'B': {Bx=B} */

static void show(const char *what, void (^body)(void))
{
    @try { body(); }
    @catch (NSException *e) { printf("%-34s %s: %s\n", what, e.name.UTF8String, e.reason.UTF8String); }
}

static NSData *archive(char code)
{
    NSMutableData *d = [NSMutableData data];
    NSArchiver *a = [[NSArchiver alloc] initForWritingWithMutableData:d];
    char t[2] = {code, 0}, v[4] = {1, 0, 1, 1}, arr[16];
    int i = 7;
    [a encodeValueOfObjCType:t at:v];
    [a encodeArrayOfObjCType:t count:4 at:v];
    snprintf(arr, sizeof arr, "{?=%ci}", code);
    PairC p = {1, 5};
    [a encodeValueOfObjCType:arr at:&p];
    snprintf(arr, sizeof arr, "%ci", code);
    [a encodeValuesOfObjCTypes:arr, v, &i];
    snprintf(arr, sizeof arr, "{Bx=%c}", code);
    [a encodeValueOfObjCType:arr at:v];
    [a encodeValueOfObjCType:"c" at:v];
    [a encodeValueOfObjCType:t at:v];
    return d;
}

int main(void)
{
    printf("@encode(BOOL)=%s @encode(Pair)=%s @encode(Bx)=%s\n", @encode(BOOL), @encode(Pair), @encode(Bx));
    for (int k = 0; k < 2; k++) {
        char code = k ? 'B' : 'c';
        NSData *d = archive(code);
        __block NSUnarchiver *u = [[NSUnarchiver alloc] initForReadingWithData:d];
        printf("archived '%c'\n", code);
        show("  value", ^{ BOOL b = NO; [u decodeValueOfObjCType:@encode(BOOL) at:&b size:sizeof b]; printf("  value %d\n", b); });
        show("  array", ^{ BOOL b[4] = {0}; [u decodeArrayOfObjCType:@encode(BOOL) count:4 at:b]; printf("  array %d%d%d%d\n", b[0], b[1], b[2], b[3]); });
        show("  struct", ^{ Pair p = {0}; [u decodeValueOfObjCType:@encode(Pair) at:&p size:sizeof p]; printf("  struct %d %d\n", p.flag, p.n); });
        show("  values", ^{ BOOL b = NO; int i = 0; [u decodeValuesOfObjCTypes:"Bi", &b, &i]; printf("  values %d %d\n", b, i); });
        show("  named struct", ^{ Bx x = {0}; [u decodeValueOfObjCType:@encode(Bx) at:&x size:sizeof x]; printf("  named struct %d\n", x.b); });
        show("  char as char", ^{ char c = 0; [u decodeValueOfObjCType:"c" at:&c size:1]; printf("  char as char %d\n", c); });
        show("  no-size value", ^{ BOOL b = NO; [u decodeValueOfObjCType:@encode(BOOL) at:&b]; printf("  no-size value %d\n", b); });
    }
    return 0;
}
