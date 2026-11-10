/* Fuzz seed: a small executable with imports, rebases, an exported symbol,
 * thread-local variables, an initializer and function starts. */
#include <stdio.h>
#include <string.h>
__thread int tlv_counter = 3;
__thread char tlv_zero[16];
int exported_value = 42;
const char *table[] = {"alpha", "beta", "gamma"};
int (*fnptr)(const char *, ...) = printf;
static int inited;
__attribute__((constructor)) static void init(void) { inited = 1; }
int helper(int x) { return x * 2 + tlv_counter; }
int main(int argc, char **argv)
{
    tlv_zero[0] = 'x';
    fnptr("%s %d %d %zu\n", table[argc % 3], helper(exported_value), inited, strlen(argv[0]));
    return 0;
}
