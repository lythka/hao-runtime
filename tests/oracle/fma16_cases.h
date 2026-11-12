/* Half fused operands shared by native and complete-state differential tests. */
#ifndef HAO_FMA16_CASES_H
#define HAO_FMA16_CASES_H
#include <stdint.h>
typedef struct { uint16_t a, b, c; } fma16_ops;
static uint64_t f16_rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static fma16_ops fma16_case(uint64_t *s, unsigned k)
{
    static const fma16_ops edge[] = {
        {0x3c00,0x1000,0x3c00}, {0x3c01,0x1000,0x3c00}, {0x3bff,0x1000,0x3c00},
        {0x3c00,0x1000,0x3c01}, {0x0400,0x3800,0}, {0x0401,0x3800,0},
        {0x0400,0x1000,0x0400}, {0x0401,0x0401,0x7bff}, {0x0401,0x0401,0xfbff},
        {0x7bff,0x3c00,0x4c00}, {0x7bff,0x3c00,0x4800}, {0x7bff,0x4000,0},
        {0x3c00,0x3c00,0xbc00}, {0,0xbc00,0}, {0x8000,0x3c00,0x8000},
        {0x0400,0x0400,0}, {0x0400,0x1000,0}, {0x7c00,0,0},
        {0x7c01,0x3c00,0}, {0x7e01,0x3c00,0}, {1,0x3c00,0}, {0x3ff,0x3c00,0}
    };
    if (k < sizeof edge / sizeof *edge) return edge[k];
    fma16_ops r = {(uint16_t)f16_rnd(s), (uint16_t)f16_rnd(s), (uint16_t)f16_rnd(s)};
    if (k & 1) {
        r.a = (r.a & 0x83ff) | (1 + f16_rnd(s) % 30) << 10;
        r.b = (r.b & 0x83ff) | (1 + f16_rnd(s) % 30) << 10;
        r.c = (r.c & 0x83ff) | (1 + f16_rnd(s) % 30) << 10;
    }
    return r;
}
#endif
