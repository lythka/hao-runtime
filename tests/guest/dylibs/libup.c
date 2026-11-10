#include "common.h"
#ifndef STAGE0
int down_hello(void);
#endif
int up_value(void) { return 7; }
static void init_body_up(void)
{
#ifndef STAGE0
    printf("  up calls down_hello=%d\n", down_hello());
#endif
}
INIT(up)
