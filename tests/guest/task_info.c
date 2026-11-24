#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(task_basic_info_data_t) == 40, "basic-info LP64 result");
_Static_assert(offsetof(task_basic_info_data_t, suspend_count) == 0, "suspend count");
_Static_assert(offsetof(task_basic_info_data_t, virtual_size) == 4, "virtual size");
_Static_assert(offsetof(task_basic_info_data_t, resident_size) == 12, "resident size");
_Static_assert(offsetof(task_basic_info_data_t, user_time) == 20, "user time");
_Static_assert(offsetof(task_basic_info_data_t, system_time) == 28, "system time");
_Static_assert(offsetof(task_basic_info_data_t, policy) == 36, "policy");
static void query(const char *name, task_t task, int flavor, unsigned capacity) {
    integer_t words[16];
    memset(words, 0x5a, sizeof words);
    mach_msg_type_number_t count = capacity;
    kern_return_t kr = task_info(task, flavor, words, &count);
    unsigned first_untouched = kr == KERN_SUCCESS ? count : 0;
    int guard = (kr != KERN_SUCCESS || count <= capacity);
    for (unsigned i = first_untouched; i < sizeof words / sizeof *words; i++)
        if (words[i] != 0x5a5a5a5a) guard = 0;
    printf("%s flavor=%d cap=%u kr=%d count=%u guard=%d", name, flavor, capacity, kr, count, guard);
    if (!kr && count == 10 && flavor != MACH_TASK_BASIC_INFO) {
        task_basic_info_data_t info;
        memcpy(&info, words, sizeof info);
        printf(" suspend=%d virtual_nonzero=%d resident_nonzero=%d policy=%d",
               info.suspend_count, info.virtual_size!=0, info.resident_size!=0, info.policy);
    }
    putchar('\n');
}
int main(void) {
    printf("ABI flavor=%d count=%lu size=%zu\n",TASK_BASIC_INFO,(unsigned long)TASK_BASIC_INFO_COUNT,sizeof(task_basic_info_data_t));
    task_t me = mach_task_self();
    query("sdk",me,TASK_BASIC_INFO,10);
    query("raw5",me,5,10);
    query("legacy-exact",me,5,8);
    query("legacy-small",me,5,7);
    query("raw18",me,18,10);
    query("mach-basic",me,MACH_TASK_BASIC_INFO,MACH_TASK_BASIC_INFO_COUNT);
    query("sdk-small",me,TASK_BASIC_INFO,9);
    query("sdk-null-port",MACH_PORT_NULL,TASK_BASIC_INFO,10);
    return 0;
}
