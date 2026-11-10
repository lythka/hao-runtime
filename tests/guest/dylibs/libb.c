#include "common.h"
static int b_state;
int b_value(void) { return b_state; }
#ifdef WITH_OPTIONAL
int b_optional(void) { return 1; }
#endif
static void init_body_b(void) { b_state = 2; }
INIT(b)
