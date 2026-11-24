/* tests/tools/identity.sh: the program whose load-time identity the stub must carry; the report comes from
 * native code (identity_helper.c). */
#include <stdio.h>

extern void idh_report(void);

__attribute__((constructor)) static void report_init(void) {
    puts("identity-event main constructor");
}

int main(void) {
    puts("identity-event main entry");
    idh_report();
    return 0;
}
