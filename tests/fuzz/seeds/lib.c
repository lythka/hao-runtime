/* Fuzz seed: a dylib whose exports trie has shared prefixes. */
#include <stdlib.h>
int foo(void) { return 1; }
int foobar(void) { return 2; }
int foobaz(void) { return 3; }
int fizz(void) { return 4; }
void *data_ptr = &foo;
void *alloc(size_t n) { return malloc(n); }
