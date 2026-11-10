#include "common.h"
int up_value(void);
static int down_state;
/* called by libup's initializer before this library's own initializer */
int down_hello(void) { printf("  down_hello (down initialised: %s)\n", down_state ? "yes" : "no"); return 5; }
int down_value(void) { return down_state + up_value(); }
static void init_body_down(void) { down_state = 1; }
INIT(down)
