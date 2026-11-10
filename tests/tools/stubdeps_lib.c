/* A dependency with an x86_64 and an arm64e slice (tests/tools/stubdeps.sh):
 * natively and under hao its initializer must run once, from whichever
 * slice is loaded at launch. */
#include <stdio.h>
static int inits;
__attribute__((constructor)) static void init(void) { inits++; }
int stubdeps_inits(void) { return inits; }
