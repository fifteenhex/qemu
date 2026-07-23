/*
 *  m68k FPU helpers
 *
 *  Copyright (c) 2006-2007 CodeSourcery
 *  Written by Paul Brook
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-loop.h"
#include "softfloat.h"

/*
 * Undefined offsets may be different on various FPU.
 * On 68040 they return 0.0 (floatx80_zero)
 */

static const floatx80 fpu_rom[128] = {
    [0x00] = make_floatx80_init(0x4000, 0xc90fdaa22168c235ULL),  /* Pi       */
    [0x0b] = make_floatx80_init(0x3ffd, 0x9a209a84fbcff798ULL),  /* Log10(2) */
    [0x0c] = make_floatx80_init(0x4000, 0xadf85458a2bb4a9aULL),  /* e        */
    [0x0d] = make_floatx80_init(0x3fff, 0xb8aa3b295c17f0bcULL),  /* Log2(e)  */
    [0x0e] = make_floatx80_init(0x3ffd, 0xde5bd8a937287195ULL),  /* Log10(e) */
    [0x0f] = make_floatx80_init(0x0000, 0x0000000000000000ULL),  /* Zero     */
    [0x30] = make_floatx80_init(0x3ffe, 0xb17217f7d1cf79acULL),  /* ln(2)    */
    [0x31] = make_floatx80_init(0x4000, 0x935d8dddaaa8ac17ULL),  /* ln(10)   */
    [0x32] = make_floatx80_init(0x3fff, 0x8000000000000000ULL),  /* 10^0     */
    [0x33] = make_floatx80_init(0x4002, 0xa000000000000000ULL),  /* 10^1     */
    [0x34] = make_floatx80_init(0x4005, 0xc800000000000000ULL),  /* 10^2     */
    [0x35] = make_floatx80_init(0x400c, 0x9c40000000000000ULL),  /* 10^4     */
    [0x36] = make_floatx80_init(0x4019, 0xbebc200000000000ULL),  /* 10^8     */
    [0x37] = make_floatx80_init(0x4034, 0x8e1bc9bf04000000ULL),  /* 10^16    */
    [0x38] = make_floatx80_init(0x4069, 0x9dc5ada82b70b59eULL),  /* 10^32    */
    [0x39] = make_floatx80_init(0x40d3, 0xc2781f49ffcfa6d5ULL),  /* 10^64    */
    [0x3a] = make_floatx80_init(0x41a8, 0x93ba47c980e98ce0ULL),  /* 10^128   */
    [0x3b] = make_floatx80_init(0x4351, 0xaa7eebfb9df9de8eULL),  /* 10^256   */
    [0x3c] = make_floatx80_init(0x46a3, 0xe319a0aea60e91c7ULL),  /* 10^512   */
    [0x3d] = make_floatx80_init(0x4d48, 0xc976758681750c17ULL),  /* 10^1024  */
    [0x3e] = make_floatx80_init(0x5a92, 0x9e8b3b5dc53d5de5ULL),  /* 10^2048  */
    [0x3f] = make_floatx80_init(0x7525, 0xc46052028a20979bULL),  /* 10^4096  */
};

int32_t HELPER(reds32)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_int32(val->d, &env->fp_status);
}

float32 HELPER(redf32)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_float32(val->d, &env->fp_status);
}

void HELPER(exts32)(CPUM68KState *env, FPReg *res, int32_t val)
{
    res->d = int32_to_floatx80(val, &env->fp_status);
}

void HELPER(extf32)(CPUM68KState *env, FPReg *res, float32 val)
{
    res->d = float32_to_floatx80(val, &env->fp_status);
}

void HELPER(extf64)(CPUM68KState *env, FPReg *res, float64 val)
{
    res->d = float64_to_floatx80(val, &env->fp_status);
}

float64 HELPER(redf64)(CPUM68KState *env, FPReg *val)
{
    return floatx80_to_float64(val->d, &env->fp_status);
}

void HELPER(firound)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round_to_int(val->d, &env->fp_status);
}

static void m68k_restore_precision_mode(CPUM68KState *env)
{
    switch (env->fpcr & FPCR_PREC_MASK) {
    case FPCR_PREC_X: /* extended */
        set_floatx80_rounding_precision(floatx80_precision_x, &env->fp_status);
        break;
    case FPCR_PREC_S: /* single */
        set_floatx80_rounding_precision(floatx80_precision_s, &env->fp_status);
        break;
    case FPCR_PREC_D: /* double */
        set_floatx80_rounding_precision(floatx80_precision_d, &env->fp_status);
        break;
    case FPCR_PREC_U: /* undefined */
    default:
        break;
    }
}

static void cf_restore_precision_mode(CPUM68KState *env)
{
    if (env->fpcr & FPCR_PREC_S) { /* single */
        set_floatx80_rounding_precision(floatx80_precision_s, &env->fp_status);
    } else { /* double */
        set_floatx80_rounding_precision(floatx80_precision_d, &env->fp_status);
    }
}

static void restore_rounding_mode(CPUM68KState *env)
{
    switch (env->fpcr & FPCR_RND_MASK) {
    case FPCR_RND_N: /* round to nearest */
        set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
        break;
    case FPCR_RND_Z: /* round to zero */
        set_float_rounding_mode(float_round_to_zero, &env->fp_status);
        break;
    case FPCR_RND_M: /* round toward minus infinity */
        set_float_rounding_mode(float_round_down, &env->fp_status);
        break;
    case FPCR_RND_P: /* round toward positive infinity */
        set_float_rounding_mode(float_round_up, &env->fp_status);
        break;
    }
}

void cpu_m68k_restore_fp_status(CPUM68KState *env)
{
    if (m68k_feature(env, M68K_FEATURE_CF_FPU)) {
        cf_restore_precision_mode(env);
    } else {
        m68k_restore_precision_mode(env);
    }
    restore_rounding_mode(env);
}

void cpu_m68k_set_fpcr(CPUM68KState *env, uint32_t val)
{
    env->fpcr = val & 0xffff;
    cpu_m68k_restore_fp_status(env);
}

void HELPER(fitrunc)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    FloatRoundMode rounding_mode = get_float_rounding_mode(&env->fp_status);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    res->d = floatx80_round_to_int(val->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
}

void HELPER(set_fpcr)(CPUM68KState *env, uint32_t val)
{
    cpu_m68k_set_fpcr(env, val);
}

/* Convert host exception flags to cpu_m68k form.  */
static int cpu_m68k_exceptbits_from_host(int host_bits)
{
    int target_bits = 0;

    if (host_bits & float_flag_invalid) {
        target_bits |= 0x80;
    }
    if (host_bits & float_flag_overflow) {
        target_bits |= 0x40;
    }
    if (host_bits & (float_flag_underflow | float_flag_output_denormal_flushed)) {
        target_bits |= 0x20;
    }
    if (host_bits & float_flag_divbyzero) {
        target_bits |= 0x10;
    }
    if (host_bits & float_flag_inexact) {
        target_bits |= 0x08;
    }
    return target_bits;
}

/*
 * Fold host float flags into the FPSR EXC byte (bits 15-8) and the
 * accrued AEXC bits, then clear them, so that each sync point sees
 * only the flags raised by the operations since the previous one.
 * The EXC byte reflects the most recent exception-raising operation.
 */
static void fp_materialize_exc(CPUM68KState *env)
{
    int now = get_float_exception_flags(&env->fp_status);
    int exc = 0;

    if (!now) {
        return;
    }
    if (now & float_flag_invalid_snan) {
        exc |= 1 << 14;                       /* SNAN */
    } else if (now & float_flag_invalid) {
        exc |= 1 << 13;                       /* OPERR */
    }
    if (now & float_flag_overflow) {
        exc |= 1 << 12;                       /* OVFL */
    }
    if (now & (float_flag_underflow | float_flag_output_denormal_flushed)) {
        exc |= 1 << 11;                       /* UNFL */
    }
    if (now & float_flag_divbyzero) {
        exc |= 1 << 10;                       /* DZ */
    }
    if (now & float_flag_inexact) {
        exc |= 1 << 9;                        /* INEX2 */
    }
    env->fpsr = (env->fpsr & ~0xff00) | exc;
    env->fp_aexc |= cpu_m68k_exceptbits_from_host(now);
    set_float_exception_flags(0, &env->fp_status);
    env->fp_trap_armed = 1;
}

uint32_t cpu_m68k_get_fpsr(CPUM68KState *env)
{
    fp_materialize_exc(env);
    return (env->fpsr & ~(0xf8)) | env->fp_aexc;
}

uint32_t HELPER(get_fpsr)(CPUM68KState *env)
{
    return cpu_m68k_get_fpsr(env);
}

void cpu_m68k_set_fpsr(CPUM68KState *env, uint32_t val)
{
    env->fpsr = val;
    env->fp_aexc = val & 0xf8;
    set_float_exception_flags(0, &env->fp_status);
    /* writing the FPSR does not create an exception trap request */
    env->fp_trap_armed = 0;
}

void HELPER(set_fpsr)(CPUM68KState *env, uint32_t val)
{
    cpu_m68k_set_fpsr(env, val);
}

#define PREC_BEGIN(prec)                                        \
    do {                                                        \
        FloatX80RoundPrec old =                                 \
            get_floatx80_rounding_precision(&env->fp_status);   \
        set_floatx80_rounding_precision(prec, &env->fp_status)  \

#define PREC_END()                                              \
        set_floatx80_rounding_precision(old, &env->fp_status);  \
    } while (0)

void HELPER(fsround)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_round(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdround)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_round(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_sqrt(val->d, &env->fp_status);
}

void HELPER(fssqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_sqrt(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdsqrt)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_sqrt(val->d, &env->fp_status);
    PREC_END();
}

void HELPER(fabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
}

void HELPER(fsabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fdabs)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_round(floatx80_abs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
}

void HELPER(fsneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fdneg)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_round(floatx80_chs(val->d), &env->fp_status);
    PREC_END();
}

void HELPER(fadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
}

void HELPER(fsadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdadd)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_add(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
}

void HELPER(fssub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdsub)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_sub(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
}

void HELPER(fsmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fdmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_mul(val0->d, val1->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsglmul)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    FloatRoundMode rounding_mode = get_float_rounding_mode(&env->fp_status);
    floatx80 a, b;

    PREC_BEGIN(floatx80_precision_s);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    a = floatx80_round(val0->d, &env->fp_status);
    b = floatx80_round(val1->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
    res->d = floatx80_mul(a, b, &env->fp_status);
    PREC_END();
}

void HELPER(fdiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
}

void HELPER(fsdiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_s);
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fddiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    PREC_BEGIN(floatx80_precision_d);
    res->d = floatx80_div(val1->d, val0->d, &env->fp_status);
    PREC_END();
}

void HELPER(fsgldiv)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    FloatRoundMode rounding_mode = get_float_rounding_mode(&env->fp_status);
    floatx80 a, b;

    PREC_BEGIN(floatx80_precision_s);
    set_float_rounding_mode(float_round_to_zero, &env->fp_status);
    a = floatx80_round(val1->d, &env->fp_status);
    b = floatx80_round(val0->d, &env->fp_status);
    set_float_rounding_mode(rounding_mode, &env->fp_status);
    res->d = floatx80_div(a, b, &env->fp_status);
    PREC_END();
}

static int float_comp_to_cc(FloatRelation float_compare)
{
    switch (float_compare) {
    case float_relation_equal:
        return FPSR_CC_Z;
    case float_relation_less:
        return FPSR_CC_N;
    case float_relation_unordered:
        return FPSR_CC_A;
    case float_relation_greater:
        return 0;
    default:
        g_assert_not_reached();
    }
}

void HELPER(fcmp)(CPUM68KState *env, FPReg *val0, FPReg *val1)
{
    FloatRelation float_compare;

    float_compare = floatx80_compare(val1->d, val0->d, &env->fp_status);
    env->fpsr = (env->fpsr & ~FPSR_CC_MASK) | float_comp_to_cc(float_compare);
}

void HELPER(ftst)(CPUM68KState *env, FPReg *val)
{
    uint32_t cc = 0;

    if (floatx80_is_neg(val->d)) {
        cc |= FPSR_CC_N;
    }

    if (floatx80_is_any_nan(val->d)) {
        cc |= FPSR_CC_A;
    } else if (floatx80_is_infinity(val->d, &env->fp_status)) {
        cc |= FPSR_CC_I;
    } else if (floatx80_is_zero(val->d)) {
        cc |= FPSR_CC_Z;
    }
    env->fpsr = (env->fpsr & ~FPSR_CC_MASK) | cc;
}

void HELPER(fconst)(CPUM68KState *env, FPReg *val, uint32_t offset)
{
    val->d = fpu_rom[offset];
}

typedef int (*float_access)(CPUM68KState *env, uint32_t addr, FPReg *fp,
                            uintptr_t ra);

static uint32_t fmovem_predec(CPUM68KState *env, uint32_t addr, uint32_t mask,
                              float_access access_fn)
{
    uintptr_t ra = GETPC();
    int i, size;

    for (i = 7; i >= 0; i--, mask <<= 1) {
        if (mask & 0x80) {
            size = access_fn(env, addr, &env->fregs[i], ra);
            if ((mask & 0xff) != 0x80) {
                addr -= size;
            }
        }
    }

    return addr;
}

static uint32_t fmovem_postinc(CPUM68KState *env, uint32_t addr, uint32_t mask,
                               float_access access_fn)
{
    uintptr_t ra = GETPC();
    int i, size;

    for (i = 0; i < 8; i++, mask <<= 1) {
        if (mask & 0x80) {
            size = access_fn(env, addr, &env->fregs[i], ra);
            addr += size;
        }
    }

    return addr;
}

static int cpu_ld_floatx80_ra(CPUM68KState *env, uint32_t addr, FPReg *fp,
                              uintptr_t ra)
{
    uint32_t high;
    uint64_t low;

    high = cpu_ldl_be_data_ra(env, addr, ra);
    low = cpu_ldq_be_data_ra(env, addr + 4, ra);

    fp->l.upper = high >> 16;
    fp->l.lower = low;

    return 12;
}

static int cpu_st_floatx80_ra(CPUM68KState *env, uint32_t addr, FPReg *fp,
                               uintptr_t ra)
{
    cpu_stl_be_data_ra(env, addr, fp->l.upper << 16, ra);
    cpu_stq_be_data_ra(env, addr + 4, fp->l.lower, ra);

    return 12;
}

static int cpu_ld_float64_ra(CPUM68KState *env, uint32_t addr, FPReg *fp,
                             uintptr_t ra)
{
    uint64_t val;

    val = cpu_ldq_be_data_ra(env, addr, ra);
    fp->d = float64_to_floatx80(*(float64 *)&val, &env->fp_status);

    return 8;
}

static int cpu_st_float64_ra(CPUM68KState *env, uint32_t addr, FPReg *fp,
                             uintptr_t ra)
{
    float64 val;

    val = floatx80_to_float64(fp->d, &env->fp_status);
    cpu_stq_be_data_ra(env, addr, *(uint64_t *)&val, ra);

    return 8;
}

uint32_t HELPER(fmovemx_st_predec)(CPUM68KState *env, uint32_t addr,
                                   uint32_t mask)
{
    return fmovem_predec(env, addr, mask, cpu_st_floatx80_ra);
}

uint32_t HELPER(fmovemx_st_postinc)(CPUM68KState *env, uint32_t addr,
                                    uint32_t mask)
{
    return fmovem_postinc(env, addr, mask, cpu_st_floatx80_ra);
}

uint32_t HELPER(fmovemx_ld_postinc)(CPUM68KState *env, uint32_t addr,
                                    uint32_t mask)
{
    return fmovem_postinc(env, addr, mask, cpu_ld_floatx80_ra);
}

uint32_t HELPER(fmovemd_st_predec)(CPUM68KState *env, uint32_t addr,
                                   uint32_t mask)
{
    return fmovem_predec(env, addr, mask, cpu_st_float64_ra);
}

uint32_t HELPER(fmovemd_st_postinc)(CPUM68KState *env, uint32_t addr,
                                    uint32_t mask)
{
    return fmovem_postinc(env, addr, mask, cpu_st_float64_ra);
}

uint32_t HELPER(fmovemd_ld_postinc)(CPUM68KState *env, uint32_t addr,
                                    uint32_t mask)
{
    return fmovem_postinc(env, addr, mask, cpu_ld_float64_ra);
}

static void make_quotient(CPUM68KState *env, int sign, uint32_t quotient)
{
    quotient = (sign << 7) | (quotient & 0x7f);
    env->fpsr = (env->fpsr & ~FPSR_QT_MASK) | (quotient << FPSR_QT_SHIFT);
}

void HELPER(fmod)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    uint64_t quotient;
    int sign = extractFloatx80Sign(val1->d) ^ extractFloatx80Sign(val0->d);

    res->d = floatx80_modrem(val1->d, val0->d, true, &quotient,
                             &env->fp_status);

    if (floatx80_is_any_nan(res->d)) {
        return;
    }

    make_quotient(env, sign, quotient);
}

void HELPER(frem)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    FPReg fp_quot;
    floatx80 fp_rem;

    fp_rem = floatx80_rem(val1->d, val0->d, &env->fp_status);
    if (!floatx80_is_any_nan(fp_rem)) {
        /* Use local temporary fp_status to set different rounding mode */
        float_status fp_status = env->fp_status;
        uint32_t quotient;
        int sign;

        /* Calculate quotient directly using round to nearest mode */
        set_float_rounding_mode(float_round_nearest_even, &fp_status);
        fp_quot.d = floatx80_div(val1->d, val0->d, &fp_status);

        sign = extractFloatx80Sign(fp_quot.d);
        quotient = floatx80_to_int32(floatx80_abs(fp_quot.d), &env->fp_status);
        make_quotient(env, sign, quotient);
    }

    res->d = fp_rem;
}

void HELPER(fgetexp)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_getexp(val->d, &env->fp_status);
}

void HELPER(fgetman)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_getman(val->d, &env->fp_status);
}

void HELPER(fscale)(CPUM68KState *env, FPReg *res, FPReg *val0, FPReg *val1)
{
    res->d = floatx80_scale(val1->d, val0->d, &env->fp_status);
}

void HELPER(flognp1)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_lognp1(val->d, &env->fp_status);
}

void HELPER(flogn)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_logn(val->d, &env->fp_status);
}

void HELPER(flog10)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_log10(val->d, &env->fp_status);
}

void HELPER(flog2)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_log2(val->d, &env->fp_status);
}

void HELPER(fetox)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_etox(val->d, &env->fp_status);
}

void HELPER(ftwotox)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_twotox(val->d, &env->fp_status);
}

void HELPER(ftentox)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_tentox(val->d, &env->fp_status);
}

void HELPER(ftan)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_tan(val->d, &env->fp_status);
}

void HELPER(fsin)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_sin(val->d, &env->fp_status);
}

void HELPER(fcos)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_cos(val->d, &env->fp_status);
}

void HELPER(fsincos)(CPUM68KState *env, FPReg *res0, FPReg *res1, FPReg *val)
{
    floatx80 a = val->d;
    /*
     * If res0 and res1 specify the same floating-point data register,
     * the sine result is stored in the register, and the cosine
     * result is discarded.
     */
    res1->d = floatx80_cos(a, &env->fp_status);
    res0->d = floatx80_sin(a, &env->fp_status);
}

void HELPER(fatan)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_atan(val->d, &env->fp_status);
}

void HELPER(fasin)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_asin(val->d, &env->fp_status);
}

void HELPER(facos)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_acos(val->d, &env->fp_status);
}

void HELPER(fatanh)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_atanh(val->d, &env->fp_status);
}

void HELPER(fetoxm1)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_etoxm1(val->d, &env->fp_status);
}

void HELPER(ftanh)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_tanh(val->d, &env->fp_status);
}

void HELPER(fsinh)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_sinh(val->d, &env->fp_status);
}

void HELPER(fcosh)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    res->d = floatx80_cosh(val->d, &env->fp_status);
}

/*
 * 68881/68882 packed decimal real format:
 *   bit 95: mantissa sign, bit 94: exponent sign,
 *   bits 91-80: 3 BCD exponent digits,
 *   bits 67-64: BCD integer digit,
 *   bits 63-0: 16 BCD fraction digits.
 * Value = (-1)^SM * D16.D15...D0 * 10^((-1)^SE * EEE)
 */

static const uint64_t pow10_tab[19] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
};

/*
 * Scale x by 10^e.  Powers up to 10^18 are exactly representable, so
 * each step is a single correctly rounded operation.
 */
static floatx80 scale10(floatx80 x, int e, float_status *st)
{
    while (e > 0) {
        int c = e > 18 ? 18 : e;
        x = floatx80_mul(x, int64_to_floatx80(pow10_tab[c], st), st);
        e -= c;
    }
    while (e < 0) {
        int c = e < -18 ? 18 : -e;
        x = floatx80_div(x, int64_to_floatx80(pow10_tab[c], st), st);
        e += c;
    }
    return x;
}

void HELPER(extpk)(CPUM68KState *env, FPReg *res, uint32_t hi, uint32_t mid,
                   uint32_t lo)
{
    float_status st = env->fp_status;
    int sm = (hi >> 31) & 1;
    int se = (hi >> 30) & 1;
    floatx80 x;
    int64_t mant;
    int exp10, i;

    if (((hi >> 16) & 0xfff) == 0xfff) {
        /* exponent field all ones: infinity or NaN */
        x.high = (sm << 15) | 0x7fff;
        if (mid == 0 && lo == 0) {
            x.low = 0;
        } else {
            x.low = (1ULL << 63) | ((uint64_t)mid << 32) | lo;
        }
        res->d = x;
        return;
    }

    exp10 = ((hi >> 24) & 0xf) * 100 + ((hi >> 20) & 0xf) * 10
          + ((hi >> 16) & 0xf);
    mant = hi & 0xf;
    for (i = 7; i >= 0; i--) {
        mant = mant * 10 + ((mid >> (i * 4)) & 0xf);
    }
    for (i = 7; i >= 0; i--) {
        mant = mant * 10 + ((lo >> (i * 4)) & 0xf);
    }

    x = int64_to_floatx80(mant, &st);
    x = scale10(x, (se ? -exp10 : exp10) - 16, &st);
    if (sm) {
        x = floatx80_chs(x);
    }
    res->d = x;
}

void HELPER(redpk)(CPUM68KState *env, FPReg *val, uint32_t addr, int32_t k)
{
    uintptr_t ra = GETPC();
    float_status st = env->fp_status;
    floatx80 x = val->d;
    int sm = (x.high >> 15) & 1;
    int exp = x.high & 0x7fff;
    uint32_t hi, mid = 0, lo = 0;

    if (exp == 0x7fff) {
        /* infinity or NaN */
        hi = ((uint32_t)sm << 31) | 0x7fff0000;
        if ((x.low << 1) != 0) {
            mid = x.low >> 32;
            lo = x.low;
        }
    } else if (exp == 0 && x.low == 0) {
        hi = (uint32_t)sm << 31;
    } else {
        floatx80 abs = x;
        floatx80 one = int64_to_floatx80(1, &st);
        int d, len, dexp, i;
        int64_t n;
        uint64_t m17, bcdm;

        abs.high &= 0x7fff;
        /* decimal exponent: refine estimate to 10^d <= abs < 10^(d+1) */
        d = (int)(((int64_t)(exp - 0x3fff) * 19728) >> 16);
        while (floatx80_lt(abs, scale10(one, d, &st), &st)) {
            d--;
        }
        while (!floatx80_lt(abs, scale10(one, d + 1, &st), &st)) {
            d++;
        }
        /* k > 0: k significant digits; k <= 0: -k digits after the point */
        len = k > 0 ? k : d + 1 - k;
        if (len < 1) {
            len = 1;
        }
        if (len > 17) {
            len = 17;
        }
        n = floatx80_to_int64(scale10(abs, len - 1 - d, &st), &st);
        if (n >= (int64_t)pow10_tab[len]) {
            /* rounding carried into an extra digit */
            n /= 10;
            d++;
        }
        /* left-align into the 17 digit mantissa d16.d15...d0 */
        m17 = (uint64_t)n * pow10_tab[17 - len];
        bcdm = 0;
        for (i = 0; i < 16; i++) {
            bcdm |= (uint64_t)(m17 % 10) << (i * 4);
            m17 /= 10;
        }
        mid = bcdm >> 32;
        lo = bcdm;
        dexp = d < 0 ? -d : d;
        hi = ((uint32_t)sm << 31) | (d < 0 ? 1u << 30 : 0)
           | (((dexp / 100) % 10) << 24) | (((dexp / 10) % 10) << 20)
           | ((dexp % 10) << 16) | (uint32_t)m17;
    }

    cpu_stl_be_data_ra(env, addr, hi, ra);
    cpu_stl_be_data_ra(env, addr + 4, mid, ra);
    cpu_stl_be_data_ra(env, addr + 8, lo, ra);
}

/*
 * 68881/68882 FSAVE/FRESTORE state frames.
 *
 * A real coprocessor dumps internal microcode state; QEMU completes
 * every instruction synchronously, so the architecturally visible
 * registers are the whole state.  We write a NULL frame when the FPU
 * is in its reset state (matching the hardware, which firmware checks)
 * and otherwise a QEMU-specific idle frame carrying the data and
 * control registers, so that a save/restore pair round-trips state --
 * 147Bug saves after FREM, restores a null frame and then the saved
 * frame, and expects the FREM result to survive.
 */
/* 8 fp regs * 12 bytes + 3 control longs + trailer */
#define FSAVE_6888X_VERSION 0x1f
#define FSAVE_6888X_PAYLOAD 112

static void fpu_do_reset(CPUM68KState *env)
{
    floatx80 nan = floatx80_default_nan(&env->fp_status);
    int i;

    for (i = 0; i < 8; i++) {
        env->fregs[i].d = nan;
    }
    cpu_m68k_set_fpcr(env, 0);
    cpu_m68k_set_fpsr(env, 0);
    env->fpiar = 0;
}

static bool fpu_in_reset_state(CPUM68KState *env)
{
    floatx80 nan = floatx80_default_nan(&env->fp_status);
    int i;

    if (env->fpcr != 0 || cpu_m68k_get_fpsr(env) != 0 || env->fpiar != 0) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        if (env->fregs[i].l.upper != (nan.high & 0xffff) ||
            env->fregs[i].l.lower != nan.low) {
            return false;
        }
    }
    return true;
}

static uint32_t do_fsave(CPUM68KState *env, uint32_t addr, uintptr_t ra)
{
    int i;

    if (fpu_in_reset_state(env)) {
        cpu_stl_be_data_ra(env, addr, 0, ra);
        return 4;
    }
    cpu_stl_be_data_ra(env, addr,
                       (FSAVE_6888X_VERSION << 24) | (FSAVE_6888X_PAYLOAD << 16) |
                       ((env->fp_trap_armed & 1) << 8),
                       ra);
    addr += 4;
    for (i = 0; i < 8; i++) {
        cpu_stl_be_data_ra(env, addr, (uint32_t)env->fregs[i].l.upper << 16, ra);
        cpu_stl_be_data_ra(env, addr + 4, env->fregs[i].l.lower >> 32, ra);
        cpu_stl_be_data_ra(env, addr + 8, env->fregs[i].l.lower, ra);
        addr += 12;
    }
    cpu_stl_be_data_ra(env, addr, env->fpcr, ra);
    cpu_stl_be_data_ra(env, addr + 4, cpu_m68k_get_fpsr(env), ra);
    cpu_stl_be_data_ra(env, addr + 8, env->fpiar, ra);
    /*
     * Trailer, mimicking a real 68882 frame whose final internal-state
     * bytes are nonzero: 147Bug's FPC test reads the last byte of a
     * frame it saved over a flag variable and relies on it being
     * nonzero.  Exception handlers may also set bits in this long.
     */
    cpu_stl_be_data_ra(env, addr + 12, 0x000000ff, ra);
    return 4 + FSAVE_6888X_PAYLOAD;
}

uint32_t HELPER(fsave_6888x)(CPUM68KState *env, uint32_t addr)
{
    return do_fsave(env, addr, GETPC());
}

uint32_t HELPER(fsave_6888x_predec)(CPUM68KState *env, uint32_t an)
{
    uintptr_t ra = GETPC();
    uint32_t size = fpu_in_reset_state(env) ? 4 : 4 + FSAVE_6888X_PAYLOAD;

    do_fsave(env, an - size, ra);
    return size;
}

uint32_t HELPER(frestore_6888x)(CPUM68KState *env, uint32_t addr)
{
    uintptr_t ra = GETPC();
    uint32_t hdr = cpu_ldl_be_data_ra(env, addr, ra);
    int i;

    if ((hdr >> 24) == 0) {
        /* NULL frame: reset the FPU */
        fpu_do_reset(env);
        return 4;
    }
    if ((hdr >> 24) == FSAVE_6888X_VERSION) {
        addr += 4;
        for (i = 0; i < 8; i++) {
            env->fregs[i].l.upper = cpu_ldl_be_data_ra(env, addr, ra) >> 16;
            env->fregs[i].l.lower =
                ((uint64_t)cpu_ldl_be_data_ra(env, addr + 4, ra) << 32) |
                cpu_ldl_be_data_ra(env, addr + 8, ra);
            addr += 12;
        }
        cpu_m68k_set_fpcr(env, cpu_ldl_be_data_ra(env, addr, ra));
        cpu_m68k_set_fpsr(env, cpu_ldl_be_data_ra(env, addr + 4, ra));
        env->fpiar = cpu_ldl_be_data_ra(env, addr + 8, ra);
        env->fp_trap_armed = (hdr >> 8) & 1;
        return 4 + FSAVE_6888X_PAYLOAD;
    }
    /* Unrecognized frame: skip it by its size byte */
    qemu_log_mask(LOG_UNIMP, "frestore: unknown frame version %02x\n",
                  hdr >> 24);
    return 4 + ((hdr >> 16) & 0xff);
}

/*
 * 6888x exception traps.  A pending exception (EXC byte & FPCR enable
 * byte) is reported when the next general FP instruction or FBcc
 * starts, like the coprocessor pre-instruction protocol.
 */
static G_NORETURN void fp_raise(CPUM68KState *env, int tt, uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = tt;
    cpu_loop_exit_restore(cs, ra);
}

void HELPER(fp_pretrap)(CPUM68KState *env)
{
    uint32_t pend;

    fp_materialize_exc(env);
    pend = env->fpsr & env->fpcr & 0xff00;
    if (!env->fp_trap_armed) {
        pend = 0;
    }
    if (unlikely(pend)) {
        int vector;

        if (pend & 0x8000) {
            vector = EXCP_FP_BSUN;
        } else if (pend & 0x4000) {
            vector = EXCP_FP_SNAN;
        } else if (pend & 0x2000) {
            vector = EXCP_FP_OPERR;
        } else if (pend & 0x1000) {
            vector = EXCP_FP_OVFL;
        } else if (pend & 0x0800) {
            vector = EXCP_FP_UNFL;
        } else if (pend & 0x0400) {
            vector = EXCP_FP_DZ;
        } else {
            vector = EXCP_FP_INEX;
        }
        /* the request is discharged by taking the trap */
        env->fp_trap_armed = 0;
        fp_raise(env, vector, GETPC());
    }
}

void HELPER(fmv)(CPUM68KState *env, FPReg *res, FPReg *val)
{
    floatx80 v = val->d;

    if ((v.high & 0x7fff) == 0 && v.low != 0) {
        /* moving a denormal signals underflow */
        float_raise(float_flag_underflow, &env->fp_status);
    } else if ((v.high & 0x7fff) == 0x7fff && (v.low << 1) != 0 &&
               !(v.low & (1ULL << 62))) {
        /* moving a signaling NaN signals SNAN and quiets it */
        float_raise(float_flag_invalid | float_flag_invalid_snan,
                    &env->fp_status);
        v.low |= 1ULL << 62;
    }
    res->d = v;
}

void HELPER(fbcc_bsun)(CPUM68KState *env)
{
    /* IEEE nonaware conditional on an unordered result */
    if (env->fpsr & FPSR_CC_A) {
        fp_materialize_exc(env);
        env->fpsr |= 1 << 15;                 /* BSUN */
        if (env->fpcr & (1 << 15)) {
            fp_raise(env, EXCP_FP_BSUN, GETPC());
        }
    }
}
