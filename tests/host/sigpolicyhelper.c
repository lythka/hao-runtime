#include <stdint.h>
/* Native helper of sigpolicyguest.c: calls a function pointer it reads
 * from memory, so hao cannot thunk it ahead and every call of a guest
 * function is one NX fault (on_fault's redirect to hao_callback_entry). */
long sph_call_slot(long (*const *slot)(long), long v) { return (*slot)(v); }
/* a fault in native code that no handler takes */
void sph_null_write(void) { *(volatile int *)(uintptr_t)16 = 1; }
