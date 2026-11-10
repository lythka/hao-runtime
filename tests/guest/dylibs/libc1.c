#include "common.h"
int c2_value(void);
int c1_value(void) { return 10; }
int c1_sum(void) { return c1_value() + c2_value(); }
static void init_body_c1(void) { printf("  c1 sees c2=%d\n", c2_value()); }
INIT(c1)
