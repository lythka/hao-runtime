/* Conversion lanes are staged in xmm11 until every lane succeeds. Normal
 * finite operands and zero are admitted on integer bits before SSE sees
 * them. All post-attempt failures restore the original MXCSR, so a late
 * saturating lane cannot leak IXC from earlier rounding into the helper.
 * No memory except the existing MXCSR scratch slot changes before commit. */
static bool t_tail_conversion(tctx *t, uint32_t i, uint64_t pc)
{
    enum { TO_INT, FROM_INT, FORMAT } kind;
    unsigned rd = BITS(i, 4, 0), rn = BITS(i, 9, 5);
    int es = 0, out = 0, n = 1, mode = 0;
    bool uns = false, gpr = false, input_high = false, narrow_high = false;
    unsigned type = BITS(i, 23, 22);
    if ((i & 0x7F20FC00) == 0x1E200000 && type != 2) {
        unsigned op = BITS(i, 20, 16);
        es = type == 0 ? 4 : type == 1 ? 8 : 2;
        out = BIT(i, 31) ? 8 : 4; gpr = true; uns = op & 1;
        if (op == 2 || op == 3) {
            if (es != 2) return false; /* existing GPR-to-S/D path */
            kind = FROM_INT;
        } else {
            switch (op & ~1u) {
            case 0: mode = RI_N; break;
            case 4: mode = RI_A; break;
            case 8: mode = RI_P; break;
            case 16: mode = RI_M; break;
            case 24: mode = RI_Z; break;
            default: return false;
            }
            if (mode == RI_Z && es != 2) return false;
            kind = TO_INT;
        }
    } else if ((i & 0xFF207C00) == 0x1E204000 && type != 2) {
        unsigned op = BITS(i, 20, 15);
        es = type == 0 ? 4 : type == 1 ? 8 : 2;
        out = op == 4 ? 4 : op == 5 ? 8 : op == 7 ? 2 : 0;
        if (!out || out == es) return false;
        kind = FORMAT;
    } else {
        unsigned key = i & 0xBFFFFC00u;
        switch (key) {
        case 0x0E79D800: case 0x2E79D800:
            kind = FROM_INT; break;
        case 0x0EF9B800: case 0x2EF9B800:
            kind = TO_INT; mode = RI_Z; break;
        case 0x0E79A800: case 0x2E79A800:
            kind = TO_INT; mode = RI_N; break;
        default: {
            bool scalar = (i & 0xDF3E0C00) == 0x5E200800;
            if (!scalar && (i & 0x9F3E0C00) != 0x0E200800) return false;
            es = BIT(i, 22) ? 8 : 4;
            if (!scalar && es == 8 && !BIT(i, 30) && BITS(i, 16, 12) != 0x16 && BITS(i, 16, 12) != 0x17) return false;
            unsigned op = BIT(i, 23) << 5 | BITS(i, 16, 12);
            if (!scalar && !BIT(i, 29) && (op == 0x16 || op == 0x17)) {
                kind = FORMAT;
                es = BIT(i, 22) ? 8 : 4;
                if (op == 0x16) { out = es / 2; n = 16 / es; narrow_high = BIT(i, 30); }
                else { out = es; es /= 2; n = 16 / out; input_high = BIT(i, 30); }
                break;
            }
            kind = TO_INT;
            switch (op) {
            case 0x1a: mode = RI_N; break;
            case 0x1b: mode = RI_M; break;
            case 0x1c: mode = RI_A; break;
            case 0x3a: mode = RI_P; break;
            case 0x3b: mode = RI_Z; break;
            case 0x1d: kind = FROM_INT; break;
            default: return false;
            }
            n = scalar ? 1 : (BIT(i, 30) ? 16 : 8) / es;
            break;
        }
        }
        if (!es) { es = 2; n = BIT(i, 30) ? 8 : 4; }
        if (kind != FORMAT) out = es;
        uns = BIT(i, 29);
    }

    fl_clobber(t);
    size_t decline[6], restore[8]; int nd = 0, nr = 0;
    x_mov_ld(&t->b, 4, RAX, xm(RBX, offsetof(hao_cpu, fpcr)));
    x_alu_ri(&t->b, XAND, 0, RAX, ~0x03c00000u);
    decline[nd++] = x_jcc32(&t->b, XCC_NE);
    if (gpr && kind == FROM_INT) {
        ldx(t, RAX, rn, out == 8, false);
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 8, RAX);
    } else vld(t, 8, rn);
    if (input_high) x_sse_shift_ri(&t->b, 0x73, 3, 8, 8);
    vzero(t, 11); vzero(t, 7); vzero(t, 12);
    if ((kind == FORMAT && out == 2) || (kind == FROM_INT && es == 2)) {
        x_movd_ld(&t->b, 13, xm(RBX, offsetof(hao_cpu, fpcr)));
        x_sse_shift_ri(&t->b, 0x73, 6, 13, 40);
        x_sse_shift_ri(&t->b, 0x73, 2, 13, 62);
    }
    x_stmxcsr(&t->b, MXCSR_SLOT);
    x_mov_ri(&t->b, R8, n);
    size_t loop = xb_len(&t->b);
    if (kind != FROM_INT) {
        /* Exponent zero with nonzero fraction, or all ones: decline. */
        x_sse_rr(&t->b, 0x66, 0x7E, es == 8, 8, RAX);
        if (es == 2) x_alu_ri(&t->b, XAND, 0, RAX, 0xffff);
        x_mov_rr(&t->b, 1, RCX, RAX);
        x_mov_ri(&t->b, RDX, es == 8 ? 0x7fffffffffffffffull : es == 4 ? 0x7fffffffull : 0x7fff);
        x_alu_rr(&t->b, XAND, 1, RCX, RDX);
        size_t zero = x_jcc32(&t->b, XCC_E);
        x_shift_ri(&t->b, XSHR, 1, RCX, es == 8 ? 52 : es == 4 ? 23 : 10);
        restore[nr++] = x_jcc32(&t->b, XCC_E);
        x_alu_ri(&t->b, XCMP, 1, RCX, es == 8 ? 2047 : es == 4 ? 255 : 31);
        restore[nr++] = x_jcc32(&t->b, XCC_E);
        x_patch32(&t->b, zero, xb_len(&t->b));
        if (es == 2) vfp_half_widen(t, 0, 8);
        else {
            x_sse_rr(&t->b, 0x66, 0x6E, es == 8, 0, RAX);
            if (es == 4) x_sse_rr(&t->b, 0xF3, 0x5A, 0, 0, 0); /* exact S -> D */
        }
        /* Only the low lane is active. Half widening also produced lane 1. */
        x_sse_rr(&t->b, 0xF3, 0x7E, 0, 0, 0);
    }
    if (kind == TO_INT) {
        static const uint8_t rounds[] = {0, 1, 2, 3};
        if (mode != RI_A) x_sse3a_rri(&t->b, 0x0B, 0, 0, rounds[mode]); /* roundsd, with PE */
        else {
            x_sse3a_rri(&t->b, 0x0B, 1, 0, 3); /* ties-away: truncate, retaining IXC */
            P(0x6F, 2, 0); x_sse_rr(&t->b, 0xF2, 0x5C, 0, 2, 1);
            vconst(t, 3, 0x7fffffffffffffffull, false); P(0xDB, 2, 3);
            vconst(t, 4, 0x3fdfffffffffffffull, false); P38(0x37, 2, 4);
            vconst(t, 4, 0x3ff0000000000000ull, false); P(0xDB, 2, 4);
            P(0xDF, 3, 0); P(0xEB, 2, 3);
            x_sse_rr(&t->b, 0xF2, 0x58, 0, 1, 2); P(0x6F, 0, 1);
        }
        x_sse_rr(&t->b, 0xF2, 0x2C, 1, RAX, 0); /* cvttsd2siq */
        x_alu_ri(&t->b, XCMP, 1, RAX, 1);
        restore[nr++] = x_jcc32(&t->b, XCC_O); /* indefinite / true INT64_MIN */
        if (uns) {
            x_mov_ri(&t->b, RCX, out == 8 ? INT64_MAX : (1ull << (8 * out)) - 1);
            x_alu_rr(&t->b, XCMP, 1, RAX, RCX);
            restore[nr++] = x_jcc32(&t->b, XCC_A);
        } else if (out != 8) {
            x_mov_rr(&t->b, 1, RCX, RAX);
            x_shift_ri(&t->b, XSHL, 1, RCX, 64 - out * 8);
            x_shift_ri(&t->b, XSAR, 1, RCX, 64 - out * 8);
            x_alu_rr(&t->b, XCMP, 1, RAX, RCX);
            restore[nr++] = x_jcc32(&t->b, XCC_NE);
        }
        x_sse_rr(&t->b, 0x66, 0x6E, 1, 0, RAX);
    } else if (kind == FROM_INT) {
        int bytes = gpr ? out : es;
        x_sse_rr(&t->b, 0x66, 0x7E, bytes == 8, 8, RAX);
        if (bytes == 2) x_alu_ri(&t->b, XAND, 0, RAX, 0xffff);
        if (!uns && bytes != 8) {
            x_shift_ri(&t->b, XSHL, 1, RAX, 64 - 8 * bytes);
            x_shift_ri(&t->b, XSAR, 1, RAX, 64 - 8 * bytes);
        }
        if (uns && bytes == 8) {
            x_test_rr(&t->b, 1, RAX, RAX);
            restore[nr++] = x_jcc32(&t->b, XCC_S);
        }
        if (es == 2 && gpr) {
            /* All 16-bit integer lanes are exact in D; GPR inputs admit
             * only this exact range to avoid double rounding to H. */
            x_mov_rr(&t->b, 1, RCX, RAX); x_alu_ri(&t->b, XADD, 1, RCX, 65536);
            x_alu_ri(&t->b, XCMP, 1, RCX, 131072);
            restore[nr++] = x_jcc32(&t->b, XCC_A);
        }
        vzero(t, 0);
        x_sse_rr(&t->b, es == 4 ? 0xF3 : 0xF2, 0x2A, 1, 0, RAX);
    }
    if ((kind == FORMAT && out == 2) || (kind == FROM_INT && es == 2)) vfp_half_round(t);
    else if (kind == FORMAT && out == 4) {
        x_sse_rr(&t->b, 0xF2, 0x5A, 0, 0, 0);
        if (es == 8) {
            /* D -> S can underflow/overflow. ARM detects tininess before
             * rounding and FZ raises UFC without IXC, unlike SSE. */
            x_sse_rr(&t->b, 0x66, 0x7E, 0, 0, RAX);
            x_shift_ri(&t->b, XSHR, 0, RAX, 23);
            x_alu_ri(&t->b, XAND, 0, RAX, 255);
            restore[nr++] = x_jcc32(&t->b, XCC_E);
            x_alu_ri(&t->b, XCMP, 0, RAX, 255);
            restore[nr++] = x_jcc32(&t->b, XCC_E);
            x_alu_ri(&t->b, XCMP, 0, RAX, 1);
            restore[nr++] = x_jcc32(&t->b, XCC_E); /* boundary may have rounded up */
        }
    }
    int result_bytes = kind == FROM_INT ? es : out;
    /* Shift in one result lane, dropping inactive high bits and preserving
     * input aliases until all lanes have committed successfully. */
    x_sse_shift_ri(&t->b, 0x73, 3, 11, result_bytes);
    x_sse_shift_ri(&t->b, 0x73, 7, 0, 16 - result_bytes);
    P(0xEB, 11, 0);
    if (n > 1) {
        x_sse_shift_ri(&t->b, 0x73, 3, 8, es);
        x_alu_ri(&t->b, XSUB, 1, R8, 1);
        size_t again = x_jcc32(&t->b, XCC_NE); x_patch32(&t->b, again, loop);
    }
    if (n * result_bytes != 16) x_sse_shift_ri(&t->b, 0x73, 3, 11, 16 - n * result_bytes);
    if ((kind == FORMAT && out == 2) || (kind == FROM_INT && es == 2)) {
        x_stmxcsr(&t->b, xm(RSP, 4)); x_movd_ld(&t->b, 0, xm(RSP, 4));
        P(0xEB, 0, 12); x_movd_st(&t->b, xm(RSP, 4), 0); x_ldmxcsr(&t->b, xm(RSP, 4));
    }
    if (gpr && kind == TO_INT) {
        x_sse_rr(&t->b, 0x66, 0x7E, out == 8, 11, RAX); stx(t, rd, RAX, false);
    } else {
        if (narrow_high) {
            x_movq_ld(&t->b, 9, VR(rd));
            x_sse_shift_ri(&t->b, 0x73, 7, 11, 8); P(0xEB, 11, 9);
        }
        fp_store(t, rd, 11);
    }
    size_t done = x_jmp32(&t->b);
    for (int j = 0; j < nr; j++) x_patch32(&t->b, restore[j], xb_len(&t->b));
    x_ldmxcsr(&t->b, MXCSR_SLOT);
    for (int j = 0; j < nd; j++) x_patch32(&t->b, decline[j], xb_len(&t->b));
    helper(t, i, pc, true); st_helper--;
    x_patch32(&t->b, done, xb_len(&t->b));
    return true;
}
