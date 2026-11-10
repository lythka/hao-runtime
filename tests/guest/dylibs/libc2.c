#include "common.h"
#ifndef STAGE0
int c1_value(void);
#endif
static int c2_state;
int c2_value(void) { return c2_state; }
static void init_body_c2(void)
{
    c2_state = 20;
#ifndef STAGE0
    printf("  c2 sees c1=%d\n", c1_value());
#endif
}
INIT(c2)
