/* tests/host/diaglog.sh: a guest that makes hao write each kind of diagnostic.
 *   diaglog nx N      an rb_tree of N nodes whose ops (guest comparators) are built at run time: native
 *                     libc calls them through the NX-fault path under HAO_NO_STRUCT_THUNKS=1; exit 0
 *   diaglog fork K N  K children each run "nx N" and exit; the parent waits
 *   diaglog fault     a native function faults on a bad pointer (hao's host fault report)
 *   diaglog abort     abort() (hao's signal report) */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rbtree.h>
#include <sys/wait.h>
#include <unistd.h>

struct node { rb_node_t rb; int key; };

static int cmp_nodes(void *ctx, const void *a, const void *b)
{
    (void)ctx;
    int x = ((const struct node *)a)->key, y = ((const struct node *)b)->key;
    return x < y ? -1 : x > y;
}

static int cmp_key(void *ctx, const void *n, const void *k)
{
    (void)ctx;
    int x = ((const struct node *)n)->key, y = *(const int *)k;
    return x < y ? -1 : x > y;
}

static int nx(int n)
{
    rb_tree_ops_t *ops = calloc(1, sizeof *ops);
    ops->rbto_compare_nodes = cmp_nodes;
    ops->rbto_compare_key = cmp_key;
    ops->rbto_node_offset = offsetof(struct node, rb);
    struct node *v = calloc((size_t)n, sizeof *v);
    rb_tree_t t;
    rb_tree_init(&t, ops);
    for (int k = 0; k < n; k++) { v[k].key = (k * 7919) % n; rb_tree_insert_node(&t, &v[k]); }
    int found = 0;
    for (int k = 0; k < n; k++) found += rb_tree_find_node(&t, &k) != NULL;
    return found != n;
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "nx";
    if (!strcmp(m, "nx")) return nx(argc > 2 ? atoi(argv[2]) : 64);
    if (!strcmp(m, "fork")) {
        int kids = argc > 2 ? atoi(argv[2]) : 4, n = argc > 3 ? atoi(argv[3]) : 64, bad = 0;
        for (int k = 0; k < kids; k++) {
            pid_t p = fork();
            if (p == 0) exit(nx(n));
            if (p < 0) bad++;
        }
        for (int st; wait(&st) > 0;) if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
        printf("fork: %d children, %d bad\n", kids, bad);
        return bad != 0;
    }
    if (!strcmp(m, "fault")) {
        volatile uintptr_t bad = 8;
        printf("%zu\n", strlen((const char *)bad));
        return 0;
    }
    if (!strcmp(m, "abort")) abort();
    return 2;
}
