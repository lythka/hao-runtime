/* Guest function pointers that reach native code by routes the call-time
 * x0-x7 scan does not see (src/fnslots.c, src/blocks.c, src/callbacks.c):
 *   1. a static callback table, and handler tables, called natively;
 *   2. pointer identity between table entries and addresses taken in code;
 *   3. C++ objects whose virtual functions native code calls;
 *   4. a block inside a structure, copied and called natively;
 *   5. a block passed as a stack argument;
 *   6. struct-returning methods added with class_addMethod and
 *      method_setImplementation, sent from native code.
 * Built without ARC (ARC would copy block literals through a route hao sees).
 * Build and run with tests/guest/callbacks.sh (native helper: fnhelper.mm);
 * compare arm64e under hao with native arm64. */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include "fnhelper.h"

/* ------------------------------------------------ 1: static callback tables */
static int sq(int x) { return x * x; }
static int tri(int x) { return x * (x + 1) / 2; }
static int neg(int x) { return -x; }
static int cube(int x) { return x * x * x; }
static long fold_sum(long acc, int x) { return acc + x; }
static long fold_hash(long acc, int x) { return acc * 33 + x; }

static const fh_ops ops_const = {"sq/sum", sq, fold_sum};          /* __DATA_CONST */
static fh_ops ops_data = {"tri/hash", tri, fold_hash};             /* __DATA */
static const fh_ops table[] = {{"sq", sq, fold_sum}, {"tri", tri, fold_sum}, {"neg", neg, fold_sum}, {"cube", cube, fold_hash}};

static void t1(void)
{
    printf("%s: %ld\n", ops_const.name, fh_run_ops(&ops_const, 500));
    printf("%s: %ld\n", ops_data.name, fh_run_ops(&ops_data, 500));
    printf("table: %ld\n", fh_run_table(table, 4, 100));
    /* guest code calls the same slots itself */
    long s = 0;
    for (int k = 0; k < 4; k++) s += table[k].fold(table[k].apply(k + 3), k);
    printf("guest calls through the table: %ld\n", s);
}

/* --------------------------------------------- 2: pointer identity (find-like) */
static int f_print(int x) { return x + 1; }
static int f_name(int x) { return x + 2; }
static int f_size(int x) { return x + 3; }
typedef struct { const char *opt; int (*fn)(int); } option;
static const option options[] = {{"-print", f_print}, {"-name", f_name}, {"-size", f_size}, {"-ls", f_print}};
static option options_rw[] = {{"-name", f_name}, {"-size", f_size}};
static volatile int vk;

static void t2(void)
{
    int (*want)(int) = f_print;          /* address taken in code */
    int n = 0;
    for (vk = 0; vk < 4; vk++) if (options[vk].fn == want) n++;
    printf("entries equal to &f_print: %d\n", n);
    vk = 1;
    printf("loaded slots compare equal: %s\n", options[vk].fn == options_rw[vk - 1].fn ? "yes" : "no");
    vk = 2;
    printf("f_size slots equal: %s, f_size via table: %d\n", options[vk].fn == options_rw[vk - 1].fn ? "yes" : "no", options[vk].fn(10));
    fh_ops t[3] = {{"a", options[0].fn, fold_sum}, {"b", options[1].fn, fold_sum}, {"c", options[2].fn, fold_sum}};
    printf("option functions called natively: %ld\n", fh_run_table(t, 3, 50));
}

/* ---------------------------------------------------------- 3: C++ vtables */
static int n_dtor;
struct Circle : FhShape {
    double r;
    explicit Circle(double r) : r(r) {}
    ~Circle() override { n_dtor++; }
    double area() const override { return 3.25 * r * r; }
    int sides() const override { return 0; }
};
struct Box4 : FhShape {
    double w, h;
    Box4(double w, double h) : w(w), h(h) {}
    ~Box4() override { n_dtor += 10; }
    double area() const override { return w * h; }
    int sides() const override { return 4; }
    long visit(long acc) const override { return acc * 7 + (long)(w + h); }
};
struct Tri : FhShape {
    double b, h;
    Tri(double b, double h) : b(b), h(h) {}
    double area() const override { return b * h / 2; }
    int sides() const override { return 3; }
};

static void t3(void)
{
    void *v[6] = {new Circle(1), new Box4(2, 3), new Tri(4, 5), new Circle(2.5), new Box4(1.5, 4), new Tri(3, 3)};
    printf("total area x200: %.3f\n", fh_total_area(v, 6, 200));
    printf("visit: %ld, sides: %d\n", fh_visit_all(v, 6), fh_sides_sum(v, 6));
    double g = 0;
    for (int k = 0; k < 6; k++) g += static_cast<FhShape *>(v[k])->area();   /* guest virtual calls */
    printf("guest area: %.3f\n", g);
    fh_delete_all(v, 6);
    printf("destructors: %d\n", n_dtor);
}

/* ------------------------------------------------- 4, 5: blocks not in x0-x7 */
static void t4(void)
{
    int base = 5;
    NSString *s = [NSString stringWithFormat:@"%d", 42];   /* captured object: copy/dispose helpers */
    /* the stack literal itself, in a heap structure: no route hao sees (MRR: no copy) */
    fh_blkbox *box = new fh_blkbox{7, ^int(int k) { return base * k + (int)s.length; }};
    printf("block in struct: %ld\n", fh_box_call(box, 300));
    __block int calls = 0;
    fh_blkbox *box2 = new fh_blkbox{1, ^int(int k) { calls++; return k ^ base; }};   /* byref helpers */
    printf("block in struct with byref: %ld, calls %d\n", fh_box_call(box2, 300), calls);
    delete box;
    delete box2;
}

static void t5(void)
{
    int m = 3;
    printf("stack block: %ld\n", fh_stack_block(1, 2, 3, 4, 5, 6, 7, 8, ^int(int k) { return k * m; }, 300));
}

/* ------------------------------------------ 6: dynamic struct-returning IMPs */
static CGRect rect_a(id self, SEL _cmd) { return CGRectMake(1, 2, 3, 4); }
static CGRect rect_b(id self, SEL _cmd) { return CGRectMake(10, 20, 30, 40); }
static CGRect scaled(id self, SEL _cmd, CGRect r, double s)
{
    return CGRectMake(r.origin.x * s, r.origin.y * s, r.size.width * s, r.size.height * s);
}
static void prect(const char *what, CGRect r)
{
    printf("%s: %g %g %g %g\n", what, r.origin.x, r.origin.y, r.size.width, r.size.height);
}

static void t6(void)
{
    Class c = objc_allocateClassPair([NSObject class], "HaoFnRect", 0);
    const char *rt = "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8";
    printf("added: %d", class_addMethod(c, @selector(haoRect), (IMP)rect_a, rt));
    printf(" %d\n", class_addMethod(c, @selector(haoScaled:by:), (IMP)scaled, "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16d48"));
    objc_registerClassPair(c);
    id o = [[c alloc] init];
    CGRect r, in = CGRectMake(1, 1, 2, 2);
    fh_rect(o, &r);
    prect("native send", r);
    fh_scaled(o, &in, 2.5, &r);
    prect("native send scaled", r);
    method_setImplementation(class_getInstanceMethod(c, @selector(haoRect)), (IMP)rect_b);
    fh_rect(o, &r);
    prect("after method_setImplementation", r);
    class_replaceMethod(c, @selector(haoRect), (IMP)rect_a, rt);
    fh_rect(o, &r);
    prect("after class_replaceMethod", r);
}

int main(int argc, char **argv)
{
    void (*tests[])(void) = {t1, t2, t3, t4, t5, t6};
    int only = argc > 1 ? atoi(argv[1]) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int k = 0; k < 6; k++) {
        if (only && only != k + 1) continue;
        printf("== %d\n", k + 1);
        @autoreleasepool { tests[k](); }
    }
    return 0;
}
