#include "common.h"
static int e_state;
int e_value(void) { return e_state; }
static void init_body_e(void) { e_state = 8; }
INIT(e)
