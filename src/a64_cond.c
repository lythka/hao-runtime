/*
 * hao — A64 condition -> x86 condition under hao's flag convention.
 *
 * EFLAGS holds the guest flags as N = SF, Z = ZF, V = OF, C = NOT CF
 * (see hao/cpu.h). Each ARM condition (Arm ARM ConditionHolds) then reads:
 *
 *   EQ  Z            ZF          E        NE  !Z           !ZF         NE
 *   CS  C            !CF         AE       CC  !C           CF          B
 *   MI  N            SF          S        PL  !N           !SF         NS
 *   VS  V            OF          O        VC  !V           !OF         NO
 *   HI  C && !Z      !CF && !ZF  A        LS  !C || Z      CF || ZF    BE
 *   GE  N == V       SF == OF    GE       LT  N != V       SF != OF    L
 *   GT  !Z && N==V   !ZF&&SF==OF G        LE  Z || N != V  ZF||SF!=OF  LE
 */
#include "hao/a64_cond.h"
#include "hao/x86.h"

static const signed char cond_xcc[16] = {
    [A64_COND_EQ] = XCC_E,  [A64_COND_NE] = XCC_NE,
    [A64_COND_CS] = XCC_AE, [A64_COND_CC] = XCC_B,
    [A64_COND_MI] = XCC_S,  [A64_COND_PL] = XCC_NS,
    [A64_COND_VS] = XCC_O,  [A64_COND_VC] = XCC_NO,
    [A64_COND_HI] = XCC_A,  [A64_COND_LS] = XCC_BE,
    [A64_COND_GE] = XCC_GE, [A64_COND_LT] = XCC_L,
    [A64_COND_GT] = XCC_G,  [A64_COND_LE] = XCC_LE,
    [A64_COND_AL] = -1,     [A64_COND_NV] = -1,
};

int a64_cond_to_xcc(unsigned cond)
{
    return cond < 16 ? cond_xcc[cond] : -1;
}
