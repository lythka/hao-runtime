/* tests/tools/identity.sh: the program whose load-time identity the stub must carry; the report comes from
 * native code (identity_helper.c). */
extern void idh_report(void);

int main(void) {
    idh_report();
    return 0;
}
