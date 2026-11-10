extern int hao_legacy_numbers[];
extern int hao_legacy_transform(int input);
__attribute__((weak)) int hao_legacy_identity = 41;
static int local = 5;
static int *volatile local_pointer = &local;
int hao_legacy_via_b(int input) {
    return hao_legacy_transform(input) + hao_legacy_numbers[1] + *local_pointer;
}
int *hao_legacy_identity_b(void) { return &hao_legacy_identity; }
