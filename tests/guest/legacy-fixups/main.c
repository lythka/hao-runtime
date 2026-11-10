#include <stdio.h>
extern int hao_legacy_numbers[];
extern int hao_legacy_transform(int);
extern int hao_legacy_via_b(int);
extern int *hao_legacy_identity_a(void);
extern int *hao_legacy_identity_b(void);
extern int hao_legacy_missing(void) __attribute__((weak_import));
static int local = 37;
static int *volatile rebase_pointer = &local;
static int *volatile addend_pointer = &hao_legacy_numbers[2];
static int (*volatile imported_function)(int) = hao_legacy_transform;
int main(void) {
    int first = hao_legacy_via_b(3);
    int second = hao_legacy_via_b(4);
    int indirect = imported_function(5);
    int *identity_a = hao_legacy_identity_a();
    int *identity_b = hao_legacy_identity_b();
    printf("rebase=%d addend=%d indirect=%d calls=%d,%d missing=%d "
           "weak_values=%d,%d weak_identity=%d stable_identity=%d\n",
           *rebase_pointer, *addend_pointer, indirect, first, second,
           hao_legacy_missing == NULL, *identity_a, *identity_b,
           identity_a == identity_b, identity_a == hao_legacy_identity_a() &&
           identity_b == hao_legacy_identity_b());
    return *rebase_pointer == 37 && *addend_pointer == 29 && indirect == 22 &&
           first == 34 && second == 37 && hao_legacy_missing == NULL ? 0 : 1;
}
