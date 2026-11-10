#include "common.h"
#include <zlib.h>
int a_value(void);
int c1_sum(void);
int down_value(void);
int b_value(void);
int re_b_value(void);
extern int b_optional(void) __attribute__((weak_import));
extern int gone_value(void) __attribute__((weak_import)); /* from a weak library that is not installed */
extern int gone_data __attribute__((weak_import));
int lz_value(void);            /* lazily loaded library */
static void init_body_main(void) {}
INIT(main)
int main(void)
{
    printf("main: a=%d c1_sum=%d down=%d b=%d re_b=%d zlib=%s\n", a_value(), c1_sum(), down_value(), b_value(),
           re_b_value(), zlibVersion()[0] ? "yes" : "no");
    printf("main: b_optional %s, gone_value %s, gone_data %s\n", b_optional ? "present" : "absent",
           gone_value ? "present" : "absent", &gone_data ? "present" : "absent");
    printf("main: before lazy call\n");
    printf("main: lz=%d\n", lz_value());
    printf("main: lz again=%d\n", lz_value());
    return 0;
}
