/* Stack arguments across the bridge. Apple arm64 passes the 9th and later
 * integer arguments packed at their natural size (an int in 4 bytes, a
 * short in 2), x86_64 SysV in 8-byte slots. mach_vm_map's last three
 * arguments (cur/max protection, inheritance) are such ints: logd's
 * mach_vm_map(..., VM_PROT_READ|VM_PROT_WRITE, VM_PROT_READ|VM_PROT_WRITE,
 * VM_INHERIT_NONE) failed on the 27.0 kernel under hao. Run by
 * tests/guest/stackargs.sh, which compares the output with native arm64. */
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#import "stackhelper.h"

static void show_region(const char *what, kern_return_t kr, mach_vm_address_t a)
{
    printf("%s kr=%d mapped=%d", what, kr, a != 0);
    if (kr == KERN_SUCCESS) {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
        mach_vm_size_t sz;
        mach_port_t o;
        mach_vm_address_t r = a;
        mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &o);
        printf(" prot=%d max=%d inherit=%d", info.protection, info.max_protection, info.inheritance);
    }
    printf("\n");
}

int main(void)
{
    @autoreleasepool {
        mach_vm_address_t a = 0;
        kern_return_t kr = mach_vm_map(mach_task_self(), &a, 0x40000, 0, VM_FLAGS_ANYWHERE | VM_MAKE_TAG(78), MACH_PORT_NULL, 0,
                                       FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        show_region("mach_vm_map(rw, rw, none)", kr, a);
        mach_vm_address_t a2 = 0;
        kr = mach_vm_map(mach_task_self(), &a2, 0x4000, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_READ,
                         VM_PROT_ALL, VM_INHERIT_COPY);
        show_region("mach_vm_map(r, rwx, copy)", kr, a2);
        vm_address_t v = 0;
        kr = vm_map(mach_task_self(), &v, 0x8000, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_READ,
                    VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_SHARE);
        show_region("vm_map(r, rw, share)", kr, v);
        mach_vm_address_t t = 0;
        vm_prot_t cur = 0, max = 0;
        kr = mach_vm_remap(mach_task_self(), &t, 0x4000, 0, VM_FLAGS_ANYWHERE, mach_task_self(), a2, FALSE, &cur, &max,
                           VM_INHERIT_NONE);
        printf("mach_vm_remap kr=%d cur=%d max=%d\n", kr, cur, max);
        show_region("  remapped", kr, t);

        /* a native method with 12 mixed-width integer arguments */
        SHStack *s = [SHStack new];
        char buf[256] = "";
        long sum = [s mix12:-5 b:-300 c:70000 d:-1099511627776L e:200 f:65000 g:-7 h:-2 i:-123456 j:'z' k:60000 l:99 buf:buf];
        printf("mix12 method: %s sum=%ld\n", buf, sum);
        double fs = [s fp10:1.5f b:2.5 c:3.5f d:4.5 e:5.5f f:6.5 g:7.5f h:8.5 i:9.5f j:10.5 n:-11 s:-12 buf:buf];
        printf("fp10 method: %s sum=%g\n", buf, fs);

        /* a native function calling a guest block with 12 mixed-width arguments */
        long r = sh_call_mix12(^long(char a1, short b, int c, long d, unsigned char e, unsigned short f, signed char g, short h,
                                    int i, char j, unsigned short k, long l) {
            printf("mix12 block: %d %d %d %ld %u %u %d %d %d %d %u %ld\n", a1, b, c, d, e, f, g, h, i, j, k, l);
            return a1 + b + c + d + e + f + g + h + i + j + k + l;
        });
        printf("mix12 block returned %ld\n", r);
    }
    return 0;
}
