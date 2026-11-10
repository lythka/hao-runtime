#include "common.h"
int b_value(void);
int e_value(void);
int lz_value(void) { return 40 + b_value() + e_value(); }
static void init_body_lz(void) { printf("  lz sees e=%d\n", e_value()); }
INIT(lz)
