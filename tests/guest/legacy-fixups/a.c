int hao_legacy_numbers[] = {7, 13, 29, 31};
__attribute__((weak)) int hao_legacy_identity = 23;
int hao_legacy_transform(int input) { return input * 3 + hao_legacy_numbers[0]; }
int *hao_legacy_identity_a(void) { return &hao_legacy_identity; }
