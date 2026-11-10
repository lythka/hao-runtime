/* Native side of tests/guest/indirect-x8.sh (fat x86_64 + arm64 + arm64e
 * dylib): C code calling guest function pointers. x86_64 clang passes the
 * struct-return buffer in rdi and calls through rax (`movq %rdi, %rax;
 * leaq buf, %rdi; callq *%rax`), so rax holds hao's thunk for the guest
 * function, not a result pointer. */
#include "x8helper.h"

__attribute__((noinline)) long x8_call_l3(struct x8_l3 (*f)(long), long k)
{
    struct x8_l3 r = f(k);
    volatile long s = r.a + r.b + r.c;   /* the call is not a tail call */
    return s != 0;
}

__attribute__((noinline)) long x8_call_int(long (*f)(long), long k)
{
    long r = f(k);
    return r + 1;
}
