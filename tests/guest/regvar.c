/* tests/guest/regvar.sh: each register-variant entry point of src/regvar_gen.inc called with a
 * Swift object in its register (regvar_arm64.s), against native arm64.
 *   regvar list        the case names
 *   regvar NAME|all    per case: the caller's mask (regvar_arm64.s), the object's retain count
 *                      before and after, and for a release variant the deinit it runs when the
 *                      count reaches zero through the variant itself (the slow path) */
#include <stdio.h>
#include <string.h>

typedef struct { long (*fn)(void *); const char *name; long isret; } rv_ent;
extern const rv_ent rv_table[], rv_table_end[];
extern void *rv_new(long);
extern long rv_rc(void *), rv_tag(void *), rv_deinits(void);
extern void rv_retain(void *), rv_release(void *);

static void one(const rv_ent *e, long k)
{
    void *p = rv_new(1000 + k);
    long d0 = rv_deinits();
    if (e->isret) {
        long rc0 = rv_rc(p), m = e->fn(p), rc1 = rv_rc(p);
        rv_release(p);
        long rc2 = rv_rc(p);
        rv_release(p);
        printf("%s mask=%#lx rc %ld -> %ld -> %ld deinit=%ld\n", e->name, m, rc0, rc1, rc2, rv_deinits() - d0);
    } else {
        rv_retain(p);
        long rc0 = rv_rc(p), m = e->fn(p), rc1 = rv_rc(p), tag = rv_tag(p);
        long m2 = e->fn(p);               /* the last reference: deinit through the variant */
        printf("%s mask=%#lx,%#lx rc %ld -> %ld tag=%ld deinit=%ld\n", e->name, m, m2, rc0, rc1, tag,
               rv_deinits() - d0);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    long n = rv_table_end - rv_table;
    if (argc > 1 && !strcmp(argv[1], "list")) {
        for (long k = 0; k < n; k++) printf("%s\n", rv_table[k].name);
        return 0;
    }
    int hit = 0;
    for (long k = 0; k < n; k++)
        if (argc < 2 || !strcmp(argv[1], "all") || !strcmp(argv[1], rv_table[k].name)) { one(&rv_table[k], k); hit = 1; }
    return hit ? 0 : 2;
}
