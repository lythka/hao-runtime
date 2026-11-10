#include "common.h"
static int d_state;
int d_value(void) { return d_state; }
static void init_body_d(void) { d_state = 4; }
INIT(d)
