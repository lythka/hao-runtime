#include "common.h"
int b_value(void);
int d_value(void);
int a_value(void) { return 100 + b_value() + d_value(); }
static void init_body_a(void) { printf("  a sees b=%d d=%d\n", b_value(), d_value()); }
INIT(a)
