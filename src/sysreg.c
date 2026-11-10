/* Default system-register hooks (runtime may override at link time). */
#include "hao/cpu.h"
#include <mach/mach_time.h>

uint64_t hao_sysreg_read(hao_cpu *cpu, unsigned enc, bool *ok)
{
    (void)cpu;
    *ok = true;
    switch (enc) {
    case 0xDF00: /* CNTFRQ_EL0 3,3,14,0,0 */ return 24000000;
    case 0xDF01: /* CNTPCT_EL0 */
    case 0xDF02: /* CNTVCT_EL0 */
    case 0xDF05: /* CNTPCTSS_EL0 */
    case 0xDF06: /* CNTVCTSS_EL0 */ {
        static mach_timebase_info_data_t tb;
        if (!tb.denom) mach_timebase_info(&tb);
        /* report a 24 MHz counter like Apple silicon */
        unsigned __int128 ns = (unsigned __int128)mach_absolute_time() * tb.numer / tb.denom;
        return (uint64_t)(ns * 24 / 1000);
    }
    case 0xD801: /* CTR_EL0 (Arm ARM D23.2.34). Darwin traps EL0 reads (SIGILL on
                  * an M4); hao answers with Apple silicon's cache geometry so
                  * code that sizes cache maintenance from it works: 64-byte
                  * I and D lines (log2 words = 4), CWG/ERG 64 bytes, PIPT
                  * instruction cache, IDC/DIC (no maintenance needed), RES1. */
        return 1ull << 31 |            /* RES1 */
               1ull << 29 |            /* DIC */
               1ull << 28 |            /* IDC */
               4ull << 24 |            /* CWG: 16 << 4 = 64 bytes */
               4ull << 20 |            /* ERG */
               4ull << 16 |            /* DminLine */
               3ull << 14 |            /* L1Ip: PIPT */
               4ull;                   /* IminLine */
    case 0xC000: /* MIDR_EL1 (emulated by Darwin for EL0) */ return 0x611f0221ull;
    default: *ok = false; return 0;
    }
}

bool hao_sysreg_write(hao_cpu *cpu, unsigned enc, uint64_t v)
{
    (void)cpu; (void)enc; (void)v;
    return false;
}
