/*
 *  ARM helper routines
 *
 *  Copyright (c) 2005-2007 CodeSourcery, LLC
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
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "helper.h"
#include "internals.h"
#include "cpu-features.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/probe.h"
#include "cpregs.h"
#include "system/memory.h"
#include "system/address-spaces.h"

#define SIGNBIT (uint32_t)0x80000000
#define SIGNBIT64 ((uint64_t)1 << 63)

int exception_target_el(CPUARMState *env)
{
    int target_el = MAX(1, arm_current_el(env));

    /*
     * No such thing as secure EL1 if EL3 is aarch32,
     * so update the target EL to EL3 in this case.
     */
    if (arm_is_secure(env) && !arm_el_is_aa64(env, 3) && target_el == 1) {
        target_el = 3;
    }

    return target_el;
}

void raise_exception(CPUARMState *env, uint32_t excp,
                     uint64_t syndrome, uint32_t target_el)
{
    CPUState *cs = env_cpu(env);

    if (target_el == 1 && (arm_hcr_el2_eff(env) & HCR_TGE)) {
        /*
         * Redirect NS EL1 exceptions to NS EL2. These are reported with
         * their original syndrome register value, with the exception of
         * SIMD/FP access traps, which are reported as uncategorized
         * (see DDI0478C.a D1.10.4)
         */
        target_el = 2;
        if (syn_get_ec(syndrome) == EC_ADVSIMDFPACCESSTRAP) {
            syndrome = syn_uncategorized();
        }
    }

    assert(!excp_is_internal(excp));
    cs->exception_index = excp;
    env->exception.syndrome = syndrome;
    env->exception.target_el = target_el;
    cpu_loop_exit(cs);
}

void raise_exception_ra(CPUARMState *env, uint32_t excp, uint64_t syndrome,
                        uint32_t target_el, uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    /*
     * restore_state_to_opc() will set env->exception.syndrome, so
     * we must restore CPU state here before setting the syndrome
     * the caller passed us, and cannot use cpu_loop_exit_restore().
     */
    cpu_restore_state(cs, ra);
    raise_exception(env, excp, syndrome, target_el);
}

uint64_t HELPER(neon_tbl)(CPUARMState *env, uint32_t desc,
                          uint64_t ireg, uint64_t def)
{
    uint64_t tmp, val = 0;
    uint32_t maxindex = ((desc & 3) + 1) * 8;
    uint32_t base_reg = desc >> 2;
    uint32_t shift, index, reg;

    for (shift = 0; shift < 64; shift += 8) {
        index = (ireg >> shift) & 0xff;
        if (index < maxindex) {
            reg = base_reg + (index >> 3);
            tmp = *aa32_vfp_dreg(env, reg);
            tmp = ((tmp >> ((index & 7) << 3)) & 0xff) << shift;
        } else {
            tmp = def & (0xffull << shift);
        }
        val |= tmp;
    }
    return val;
}

void HELPER(v8m_stackcheck)(CPUARMState *env, uint32_t newvalue)
{
    /*
     * Perform the v8M stack limit check for SP updates from translated code,
     * raising an exception if the limit is breached.
     */
    if (newvalue < v7m_sp_limit(env)) {
        /*
         * Stack limit exceptions are a rare case, so rather than syncing
         * PC/condbits before the call, we use raise_exception_ra() so
         * that cpu_restore_state() will sort them out.
         */
        raise_exception_ra(env, EXCP_STKOF, 0, 1, GETPC());
    }
}

/* Sign/zero extend */
uint32_t HELPER(sxtb16)(uint32_t x)
{
    uint32_t res;
    res = (uint16_t)(int8_t)x;
    res |= (uint32_t)(int8_t)(x >> 16) << 16;
    return res;
}

static void handle_possible_div0_trap(CPUARMState *env, uintptr_t ra)
{
    /*
     * Take a division-by-zero exception if necessary; otherwise return
     * to get the usual non-trapping division behaviour (result of 0)
     */
    if (arm_feature(env, ARM_FEATURE_M)
        && (env->v7m.ccr[env->v7m.secure] & R_V7M_CCR_DIV_0_TRP_MASK)) {
        raise_exception_ra(env, EXCP_DIVBYZERO, 0, 1, ra);
    }
}

uint32_t HELPER(uxtb16)(uint32_t x)
{
    uint32_t res;
    res = (uint16_t)(uint8_t)x;
    res |= (uint32_t)(uint8_t)(x >> 16) << 16;
    return res;
}

int32_t HELPER(sdiv)(CPUARMState *env, int32_t num, int32_t den)
{
    if (den == 0) {
        handle_possible_div0_trap(env, GETPC());
        return 0;
    }
    if (num == INT_MIN && den == -1) {
        return INT_MIN;
    }
    return num / den;
}

uint32_t HELPER(udiv)(CPUARMState *env, uint32_t num, uint32_t den)
{
    if (den == 0) {
        handle_possible_div0_trap(env, GETPC());
        return 0;
    }
    return num / den;
}

uint32_t HELPER(rbit)(uint32_t x)
{
    return revbit32(x);
}

uint32_t HELPER(add_setq)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (((res ^ a) & SIGNBIT) && !((a ^ b) & SIGNBIT))
        env->QF = 1;
    return res;
}

uint32_t HELPER(add_saturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (((res ^ a) & SIGNBIT) && !((a ^ b) & SIGNBIT)) {
        env->QF = 1;
        res = ~(((int32_t)a >> 31) ^ SIGNBIT);
    }
    return res;
}

uint32_t HELPER(sub_saturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a - b;
    if (((res ^ a) & SIGNBIT) && ((a ^ b) & SIGNBIT)) {
        env->QF = 1;
        res = ~(((int32_t)a >> 31) ^ SIGNBIT);
    }
    return res;
}

uint32_t HELPER(add_usaturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (res < a) {
        env->QF = 1;
        res = ~0;
    }
    return res;
}

uint32_t HELPER(sub_usaturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a - b;
    if (res > a) {
        env->QF = 1;
        res = 0;
    }
    return res;
}

/* Signed saturation.  */
static inline uint32_t do_ssat(CPUARMState *env, int32_t val, int shift)
{
    int32_t top;
    uint32_t mask;

    top = val >> shift;
    mask = (1u << shift) - 1;
    if (top > 0) {
        env->QF = 1;
        return mask;
    } else if (top < -1) {
        env->QF = 1;
        return ~mask;
    }
    return val;
}

/* Unsigned saturation.  */
static inline uint32_t do_usat(CPUARMState *env, int32_t val, int shift)
{
    uint32_t max;

    max = (1u << shift) - 1;
    if (val < 0) {
        env->QF = 1;
        return 0;
    } else if (val > max) {
        env->QF = 1;
        return max;
    }
    return val;
}

/* Signed saturate.  */
uint32_t HELPER(ssat)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    return do_ssat(env, x, shift);
}

/* Dual halfword signed saturate.  */
uint32_t HELPER(ssat16)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    uint32_t res;

    res = (uint16_t)do_ssat(env, (int16_t)x, shift);
    res |= do_ssat(env, ((int32_t)x) >> 16, shift) << 16;
    return res;
}

/* Unsigned saturate.  */
uint32_t HELPER(usat)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    return do_usat(env, x, shift);
}

/* Dual halfword unsigned saturate.  */
uint32_t HELPER(usat16)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    uint32_t res;

    res = (uint16_t)do_usat(env, (int16_t)x, shift);
    res |= do_usat(env, ((int32_t)x) >> 16, shift) << 16;
    return res;
}

void HELPER(setend)(CPUARMState *env)
{
    env->uncached_cpsr ^= CPSR_E;
    arm_rebuild_hflags(env);
}

void HELPER(check_bxj_trap)(CPUARMState *env, uint32_t rm)
{
    /*
     * Only called if in NS EL0 or EL1 for a BXJ for a v7A CPU;
     * check if HSTR.TJDBX means we need to trap to EL2.
     */
    if (env->cp15.hstr_el2 & HSTR_TJDBX) {
        /*
         * We know the condition code check passed, so take the IMPDEF
         * choice to always report CV=1 COND 0xe
         */
        uint32_t syn = syn_bxjtrap(1, 0xe, rm);
        raise_exception_ra(env, EXCP_HYP_TRAP, syn, 2, GETPC());
    }
}

#ifndef CONFIG_USER_ONLY
/*
 * Function checks whether WFx (WFI/WFE) instructions are set up to be trapped.
 * The function returns the target EL (1-3) if the instruction is to be trapped;
 * otherwise it returns 0 indicating it is not trapped.
 * For a trap, *excp is updated with the EXCP_* trap type to use.
 */
static inline int check_wfx_trap(CPUARMState *env, bool is_wfe, uint32_t *excp)
{
    int cur_el = arm_current_el(env);
    uint64_t mask;

    *excp = EXCP_UDEF;

    if (arm_feature(env, ARM_FEATURE_M)) {
        /* M profile cores can never trap WFI/WFE. */
        return 0;
    }

    /* If we are currently in EL0 then we need to check if SCTLR is set up for
     * WFx instructions being trapped to EL1. These trap bits don't exist in v7.
     */
    if (cur_el < 1 && arm_feature(env, ARM_FEATURE_V8)) {
        mask = is_wfe ? SCTLR_nTWE : SCTLR_nTWI;
        if (!(arm_sctlr(env, cur_el) & mask)) {
            return exception_target_el(env);
        }
    }

    /* We are not trapping to EL1; trap to EL2 if HCR_EL2 requires it
     * No need for ARM_FEATURE check as if HCR_EL2 doesn't exist the
     * bits will be zero indicating no trap.
     */
    if (cur_el < 2) {
        mask = is_wfe ? HCR_TWE : HCR_TWI;
        if (arm_hcr_el2_eff(env) & mask) {
            return 2;
        }
    }

    /* We are not trapping to EL1 or EL2; trap to EL3 if SCR_EL3 requires it */
    if (arm_feature(env, ARM_FEATURE_V8) && !arm_is_el3_or_mon(env)) {
        mask = (is_wfe) ? SCR_TWE : SCR_TWI;
        if (env->cp15.scr_el3 & mask) {
            if (!arm_el_is_aa64(env, 3)) {
                *excp = EXCP_MON_TRAP;
            }
            return 3;
        }
    }

    return 0;
}
#endif

void HELPER(wfi)(CPUARMState *env, uint32_t insn_len)
{
#ifdef CONFIG_USER_ONLY
    /*
     * WFI in the user-mode emulator is technically permitted but not
     * something any real-world code would do. AArch64 Linux kernels
     * trap it via SCTRL_EL1.nTWI and make it an (expensive) NOP;
     * AArch32 kernels don't trap it so it will delay a bit.
     * For QEMU, make it NOP here, because trying to raise EXCP_HLT
     * would trigger an abort.
     */
    return;
#else
    CPUState *cs = env_cpu(env);
    uint32_t excp;
    int target_el = check_wfx_trap(env, false, &excp);

    if (cpu_has_work(cs)) {
        /* Don't bother to go into our "low power state" if
         * we would just wake up immediately.
         */
        return;
    }

    if (target_el) {
        if (env->aarch64) {
            env->pc -= insn_len;
        } else {
            env->regs[15] -= insn_len;
        }

        raise_exception(env, excp, syn_wfx(1, 0xe, 0, false, WFI, insn_len == 2),
                        target_el);
    }

    env->halt_reason = HALT_WFI;
    cs->exception_index = EXCP_HLT;
    cs->halted = 1;
    cpu_loop_exit(cs);
#endif
}

void HELPER(wfit)(CPUARMState *env, uint32_t rd)
{
#ifdef CONFIG_USER_ONLY
    /*
     * WFI in the user-mode emulator is technically permitted but not
     * something any real-world code would do. AArch64 Linux kernels
     * trap it via SCTRL_EL1.nTWI and make it an (expensive) NOP;
     * AArch32 kernels don't trap it so it will delay a bit.
     * For QEMU, make it NOP here, because trying to raise EXCP_HLT
     * would trigger an abort.
     */
    return;
#else
    ARMCPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);
    uint32_t excp;
    int target_el = check_wfx_trap(env, false, &excp);
    /* The WFIT should time out when CNTVCT_EL0 >= the specified value. */
    uint64_t cntval = gt_get_countervalue(env);
    uint64_t timeout = env->xregs[rd];
    /*
     * We want the value that we would get if we read CNTVCT_EL0 from
     * the current exception level, so the direct_access offset, not
     * the indirect_access one. Compare the pseudocode LocalTimeoutEvent(),
     * which calls VirtualCounterTimer().
     */
    uint64_t offset = gt_direct_access_timer_offset(env, GTIMER_VIRT);
    uint64_t cntvct = cntval - offset;
    uint64_t nexttick;

    if (cpu_has_work(cs) || cntvct >= timeout) {
        /*
         * Don't bother to go into our "low power state" if
         * we would just wake up immediately.
         */
        return;
    }

    if (target_el) {
        env->pc -= 4;
        raise_exception(env, excp, syn_wfx(1, 0xe, rd, true, WFIT, false), target_el);
    }

    if (uadd64_overflow(timeout, offset, &nexttick)) {
        nexttick = UINT64_MAX;
    }
    if (nexttick > INT64_MAX / gt_cntfrq_period_ns(cpu)) {
        /*
         * If the timeout is too long for the signed 64-bit range
         * of a QEMUTimer, let it expire early.
         */
        timer_mod_ns(cpu->wfxt_timer, INT64_MAX);
    } else {
        timer_mod(cpu->wfxt_timer, nexttick);
    }
    env->halt_reason = HALT_WFI;
    cs->exception_index = EXCP_HLT;
    cs->halted = 1;
    cpu_loop_exit(cs);
#endif
}

void HELPER(sev)(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);
    CPU_FOREACH(cs) {
        ARMCPU *target_cpu = ARM_CPU(cs);
        if (arm_feature(&target_cpu->env, ARM_FEATURE_M)) {
            target_cpu->env.event_register = true;
        }
        if (!qemu_cpu_is_self(cs)) {
            qemu_cpu_kick(cs);
        }
    }
}

void HELPER(wfe)(CPUARMState *env)
{
#ifdef CONFIG_USER_ONLY
    /*
     * WFE in the user-mode emulator is a NOP. Real-world user-mode code
     * shouldn't execute WFE, but if it does, we make it a NOP rather than
     * aborting when we try to raise EXCP_HLT.
     */
    return;
#else
    /*
     * WFE (Wait For Event) is a hint instruction.
     * For Cortex-M (M-profile), we implement the strict architectural behavior:
     * 1. Check the Event Register (set by SEV or SEVONPEND).
     * 2. If set, clear it and continue (consume the event).
     */
    if (arm_feature(env, ARM_FEATURE_M)) {
        CPUState *cs = env_cpu(env);

        if (env->event_register) {
            env->event_register = false;
            return;
        }

        env->halt_reason = HALT_WFE;
        cs->exception_index = EXCP_HLT;
        cs->halted = 1;
        cpu_loop_exit(cs);
    } else {
        /*
         * For A-profile and others, we rely on the existing "yield" behavior.
         * Don't actually halt the CPU, just yield back to top
         * level loop. This is not going into a "low power state"
         * (ie halting until some event occurs), so we never take
         * a configurable trap to a different exception level
         */
        HELPER(yield)(env);
    }
#endif
}

void HELPER(yield)(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);

    /* This is a non-trappable hint instruction that generally indicates
     * that the guest is currently busy-looping. Yield control back to the
     * top level loop so that a more deserving VCPU has a chance to run.
     */
    cs->exception_index = EXCP_YIELD;
    cpu_loop_exit(cs);
}

/* Raise an internal-to-QEMU exception. This is limited to only
 * those EXCP values which are special cases for QEMU to interrupt
 * execution and not to be used for exceptions which are passed to
 * the guest (those must all have syndrome information and thus should
 * use exception_with_syndrome*).
 */
void HELPER(exception_internal)(CPUARMState *env, uint32_t excp)
{
    CPUState *cs = env_cpu(env);

    assert(excp_is_internal(excp));
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}

/* Raise an exception with the specified syndrome register value */
void HELPER(exception_with_syndrome_el)(CPUARMState *env, uint32_t excp,
                                        uint32_t syndrome, uint32_t target_el)
{
    raise_exception(env, excp, syndrome, target_el);
}

/*
 * Raise an exception with the specified syndrome register value
 * to the default target el.
 */
void HELPER(exception_with_syndrome)(CPUARMState *env, uint32_t excp,
                                     uint32_t syndrome)
{
    raise_exception(env, excp, syndrome, exception_target_el(env));
}

uint32_t HELPER(cpsr_read)(CPUARMState *env)
{
    return cpsr_read(env) & ~CPSR_EXEC;
}

void HELPER(cpsr_write)(CPUARMState *env, uint32_t val, uint32_t mask)
{
    cpsr_write(env, val, mask, CPSRWriteByInstr);
    /* TODO: Not all cpsr bits are relevant to hflags.  */
    arm_rebuild_hflags(env);
}

/* Write the CPSR for a 32-bit exception return */
void HELPER(cpsr_write_eret)(CPUARMState *env, uint32_t val)
{
    uint32_t mask;

    bql_lock();
    arm_call_pre_el_change_hook(env_archcpu(env));
    bql_unlock();

    mask = aarch32_cpsr_valid_mask(env->features, &env_archcpu(env)->isar);
    cpsr_write(env, val, mask, CPSRWriteExceptionReturn);

    /* Generated code has already stored the new PC value, but
     * without masking out its low bits, because which bits need
     * masking depends on whether we're returning to Thumb or ARM
     * state. Do the masking now.
     */
    env->regs[15] &= (env->thumb ? ~1 : ~3);
    arm_rebuild_hflags(env);

    bql_lock();
    arm_call_el_change_hook(env_archcpu(env));
    bql_unlock();
}

/* Access to user mode registers from privileged modes.  */
uint32_t HELPER(get_user_reg)(CPUARMState *env, uint32_t regno)
{
    uint32_t val;

    if (regno == 13) {
        val = env->banked_r13[BANK_USRSYS];
    } else if (regno == 14) {
        val = env->banked_r14[BANK_USRSYS];
    } else if (regno >= 8
               && (env->uncached_cpsr & 0x1f) == ARM_CPU_MODE_FIQ) {
        val = env->usr_regs[regno - 8];
    } else {
        val = env->regs[regno];
    }
    return val;
}

void HELPER(set_user_reg)(CPUARMState *env, uint32_t regno, uint32_t val)
{
    if (regno == 13) {
        env->banked_r13[BANK_USRSYS] = val;
    } else if (regno == 14) {
        env->banked_r14[BANK_USRSYS] = val;
    } else if (regno >= 8
               && (env->uncached_cpsr & 0x1f) == ARM_CPU_MODE_FIQ) {
        env->usr_regs[regno - 8] = val;
    } else {
        env->regs[regno] = val;
    }
}

void HELPER(set_r13_banked)(CPUARMState *env, uint32_t mode, uint32_t val)
{
    if ((env->uncached_cpsr & CPSR_M) == mode) {
        env->regs[13] = val;
    } else {
        env->banked_r13[bank_number(mode)] = val;
    }
}

uint32_t HELPER(get_r13_banked)(CPUARMState *env, uint32_t mode)
{
    if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_SYS) {
        /* SRS instruction is UNPREDICTABLE from System mode; we UNDEF.
         * Other UNPREDICTABLE and UNDEF cases were caught at translate time.
         */
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }

    if ((env->uncached_cpsr & CPSR_M) == mode) {
        return env->regs[13];
    } else {
        return env->banked_r13[bank_number(mode)];
    }
}

static void msr_mrs_banked_exc_checks(CPUARMState *env, uint32_t tgtmode,
                                      uint32_t regno)
{
    /* Raise an exception if the requested access is one of the UNPREDICTABLE
     * cases; otherwise return. This broadly corresponds to the pseudocode
     * BankedRegisterAccessValid() and SPSRAccessValid(),
     * except that we have already handled some cases at translate time.
     */
    int curmode = env->uncached_cpsr & CPSR_M;

    if (tgtmode == ARM_CPU_MODE_HYP) {
        /*
         * Handle Hyp target regs first because some are special cases
         * which don't want the usual "not accessible from tgtmode" check.
         */
        switch (regno) {
        case 16 ... 17: /* ELR_Hyp, SPSR_Hyp */
            if (curmode != ARM_CPU_MODE_HYP && curmode != ARM_CPU_MODE_MON) {
                goto undef;
            }
            break;
        case 13:
            if (curmode != ARM_CPU_MODE_MON) {
                goto undef;
            }
            break;
        default:
            g_assert_not_reached();
        }
        return;
    }

    if (curmode == tgtmode) {
        goto undef;
    }

    if (tgtmode == ARM_CPU_MODE_USR) {
        switch (regno) {
        case 8 ... 12:
            if (curmode != ARM_CPU_MODE_FIQ) {
                goto undef;
            }
            break;
        case 13:
            if (curmode == ARM_CPU_MODE_SYS) {
                goto undef;
            }
            break;
        case 14:
            if (curmode == ARM_CPU_MODE_HYP || curmode == ARM_CPU_MODE_SYS) {
                goto undef;
            }
            break;
        default:
            break;
        }
    }

    return;

undef:
    raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                    exception_target_el(env));
}

void HELPER(msr_banked)(CPUARMState *env, uint32_t value, uint32_t tgtmode,
                        uint32_t regno)
{
    msr_mrs_banked_exc_checks(env, tgtmode, regno);

    switch (regno) {
    case 16: /* SPSRs */
        if (tgtmode == (env->uncached_cpsr & CPSR_M)) {
            /* Only happens for SPSR_Hyp access in Hyp mode */
            env->spsr = value;
        } else {
            env->banked_spsr[bank_number(tgtmode)] = value;
        }
        break;
    case 17: /* ELR_Hyp */
        env->elr_el[2] = value;
        break;
    case 13:
        env->banked_r13[bank_number(tgtmode)] = value;
        break;
    case 14:
        env->banked_r14[r14_bank_number(tgtmode)] = value;
        break;
    case 8 ... 12:
        switch (tgtmode) {
        case ARM_CPU_MODE_USR:
            env->usr_regs[regno - 8] = value;
            break;
        case ARM_CPU_MODE_FIQ:
            env->fiq_regs[regno - 8] = value;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    default:
        g_assert_not_reached();
    }
}

uint32_t HELPER(mrs_banked)(CPUARMState *env, uint32_t tgtmode, uint32_t regno)
{
    msr_mrs_banked_exc_checks(env, tgtmode, regno);

    switch (regno) {
    case 16: /* SPSRs */
        if (tgtmode == (env->uncached_cpsr & CPSR_M)) {
            /* Only happens for SPSR_Hyp access in Hyp mode */
            return env->spsr;
        } else {
            return env->banked_spsr[bank_number(tgtmode)];
        }
    case 17: /* ELR_Hyp */
        return env->elr_el[2];
    case 13:
        return env->banked_r13[bank_number(tgtmode)];
    case 14:
        return env->banked_r14[r14_bank_number(tgtmode)];
    case 8 ... 12:
        switch (tgtmode) {
        case ARM_CPU_MODE_USR:
            return env->usr_regs[regno - 8];
        case ARM_CPU_MODE_FIQ:
            return env->fiq_regs[regno - 8];
        default:
            g_assert_not_reached();
        }
    default:
        g_assert_not_reached();
    }
}

const void *HELPER(access_check_cp_reg)(CPUARMState *env, uint32_t key,
                                        uint32_t syndrome, uint32_t isread)
{
    ARMCPU *cpu = env_archcpu(env);
    const ARMCPRegInfo *ri = get_arm_cp_reginfo(cpu->cp_regs, key);
    CPAccessResult res = CP_ACCESS_OK;
    int target_el;
    uint32_t excp;

    assert(ri != NULL);

    if (ri->accessfn) {
        res = ri->accessfn(env, ri, isread);
    }

    /*
     * If the access function indicates a trap from EL0 to EL1 then
     * that always takes priority over the HSTR_EL2 trap. (If it indicates
     * a trap to EL3, then the HSTR_EL2 trap takes priority; if it indicates
     * a trap to EL2, then the syndrome is the same either way so we don't
     * care whether technically the architecture says that HSTR_EL2 trap or
     * the other trap takes priority. So we take the "check HSTR_EL2" path
     * for all of those cases.)
     */
    if (res != CP_ACCESS_OK && ((res & CP_ACCESS_EL_MASK) < 2) &&
        arm_current_el(env) == 0) {
        goto fail;
    }

    /*
     * HSTR_EL2 traps from EL1 are checked earlier, in generated code;
     * we only need to check here for traps from EL0.
     */
    if (!is_a64(env) && arm_current_el(env) == 0 && ri->cp == 15 &&
        arm_is_el2_enabled(env) &&
        (arm_hcr_el2_eff(env) & (HCR_E2H | HCR_TGE)) != (HCR_E2H | HCR_TGE)) {
        uint32_t mask = 1 << ri->crn;

        if (ri->type & ARM_CP_64BIT) {
            mask = 1 << ri->crm;
        }

        /* T4 and T14 are RES0 */
        mask &= ~((1 << 4) | (1 << 14));

        if (env->cp15.hstr_el2 & mask) {
            res = CP_ACCESS_TRAP_EL2;
            goto fail;
        }
    }

    /*
     * Fine-grained traps also are lower priority than undef-to-EL1,
     * higher priority than trap-to-EL3, and we don't care about priority
     * order with other EL2 traps because the syndrome value is the same.
     */
    if (arm_fgt_active(env, arm_current_el(env))) {
        uint64_t trapword = 0;
        unsigned int idx = FIELD_EX32(ri->fgt, FGT, IDX);
        unsigned int bitpos = FIELD_EX32(ri->fgt, FGT, BITPOS);
        bool rev = FIELD_EX32(ri->fgt, FGT, REV);
        bool nxs = FIELD_EX32(ri->fgt, FGT, NXS);
        bool trapbit;

        if (ri->fgt & FGT_EXEC) {
            assert(idx < ARRAY_SIZE(env->cp15.fgt_exec));
            trapword = env->cp15.fgt_exec[idx];
        } else if (isread && (ri->fgt & FGT_R)) {
            assert(idx < ARRAY_SIZE(env->cp15.fgt_read));
            trapword = env->cp15.fgt_read[idx];
        } else if (!isread && (ri->fgt & FGT_W)) {
            assert(idx < ARRAY_SIZE(env->cp15.fgt_write));
            trapword = env->cp15.fgt_write[idx];
        }

        if (nxs && (arm_hcrx_el2_eff(env) & HCRX_FGTNXS)) {
            /*
             * If HCRX_EL2.FGTnXS is 1 then the fine-grained trap for
             * TLBI maintenance insns does *not* apply to the nXS variant.
             */
            trapbit = 0;
        } else {
            trapbit = extract64(trapword, bitpos, 1);
        }
        if (trapbit != rev) {
            res = CP_ACCESS_TRAP_EL2;
            goto fail;
        }
    }

    if (likely(res == CP_ACCESS_OK)) {
        return ri;
    }

 fail:
    excp = EXCP_UDEF;
    switch (res) {
        /* CP_ACCESS_TRAP* traps are always direct to a specified EL */
    case CP_ACCESS_TRAP_EL3:
        /*
         * If EL3 is AArch32 then there's no syndrome register; the cases
         * where we would raise a SystemAccessTrap to AArch64 EL3 all become
         * raising a Monitor trap exception. (Because there's no visible
         * syndrome it doesn't matter what we pass to raise_exception().)
         */
        if (!arm_el_is_aa64(env, 3)) {
            excp = EXCP_MON_TRAP;
        }
        break;
    case CP_ACCESS_TRAP_EL2:
    case CP_ACCESS_TRAP_EL1:
        break;
    case CP_ACCESS_UNDEFINED:
        /* CP_ACCESS_UNDEFINED is never direct to a specified EL */
        if (cpu_isar_feature(aa64_ids, cpu) && isread &&
            arm_cpreg_in_idspace(ri)) {
            /*
             * FEAT_IDST says this should be reported as EC_SYSTEMREGISTERTRAP,
             * not EC_UNCATEGORIZED
             */
            break;
        }
        syndrome = syn_uncategorized();
        break;
    case CP_ACCESS_EXLOCK:
        /*
         * CP_ACCESS_EXLOCK is always directed to the current EL,
         * which is going to be the same as the usual target EL.
         */
        syndrome = syn_gcs_exlock();
        break;
    default:
        g_assert_not_reached();
    }

    target_el = res & CP_ACCESS_EL_MASK;
    switch (target_el) {
    case 0:
        target_el = exception_target_el(env);
        break;
    case 1:
        assert(arm_current_el(env) < 2);
        break;
    case 2:
        assert(arm_current_el(env) != 3);
        assert(arm_is_el2_enabled(env));
        break;
    case 3:
        assert(arm_feature(env, ARM_FEATURE_EL3));
        break;
    default:
        g_assert_not_reached();
    }

    raise_exception(env, excp, syndrome, target_el);
}

const void *HELPER(lookup_cp_reg)(CPUARMState *env, uint32_t key)
{
    ARMCPU *cpu = env_archcpu(env);
    const ARMCPRegInfo *ri = get_arm_cp_reginfo(cpu->cp_regs, key);

    assert(ri != NULL);
    return ri;
}

/*
 * Test for HCR_EL2.TIDCP at EL1.
 * Since implementation defined registers are rare, and within QEMU
 * most of them are no-op, do not waste HFLAGS space for this and
 * always use a helper.
 */
void HELPER(tidcp_el1)(CPUARMState *env, uint32_t syndrome)
{
    if (arm_hcr_el2_eff(env) & HCR_TIDCP) {
        raise_exception_ra(env, EXCP_UDEF, syndrome, 2, GETPC());
    }
}

/*
 * Similarly, for FEAT_TIDCP1 at EL0.
 * We have already checked for the presence of the feature.
 */
void HELPER(tidcp_el0)(CPUARMState *env, uint32_t syndrome)
{
    /* See arm_sctlr(), but we also need the sctlr el. */
    ARMMMUIdx mmu_idx = arm_mmu_idx_el(env, 0);
    int target_el;

    switch (mmu_idx) {
    case ARMMMUIdx_E20_0:
        target_el = 2;
        break;
    case ARMMMUIdx_E30_0:
        target_el = 3;
        break;
    default:
        target_el = 1;
        break;
    }

    /*
     * The bit is not valid unless the target el is aa64, but since the
     * bit test is simpler perform that first and check validity after.
     */
    if ((env->cp15.sctlr_el[target_el] & SCTLR_TIDCP)
        && arm_el_is_aa64(env, target_el)) {
        raise_exception_ra(env, EXCP_UDEF, syndrome, target_el, GETPC());
    }
}

void HELPER(set_cp_reg)(CPUARMState *env, const void *rip, uint32_t value)
{
    const ARMCPRegInfo *ri = rip;

    if (ri->type & ARM_CP_IO) {
        bql_lock();
        ri->writefn(env, ri, value);
        bql_unlock();
    } else {
        ri->writefn(env, ri, value);
    }
}

uint32_t HELPER(get_cp_reg)(CPUARMState *env, const void *rip)
{
    const ARMCPRegInfo *ri = rip;
    uint32_t res;

    if (ri->type & ARM_CP_IO) {
        bql_lock();
        res = ri->readfn(env, ri);
        bql_unlock();
    } else {
        res = ri->readfn(env, ri);
    }

    return res;
}

void HELPER(set_cp_reg64)(CPUARMState *env, const void *rip, uint64_t value)
{
    const ARMCPRegInfo *ri = rip;

    if (ri->type & ARM_CP_IO) {
        bql_lock();
        ri->writefn(env, ri, value);
        bql_unlock();
    } else {
        ri->writefn(env, ri, value);
    }
}

uint64_t HELPER(get_cp_reg64)(CPUARMState *env, const void *rip)
{
    const ARMCPRegInfo *ri = rip;
    uint64_t res;

    if (ri->type & ARM_CP_IO) {
        bql_lock();
        res = ri->readfn(env, ri);
        bql_unlock();
    } else {
        res = ri->readfn(env, ri);
    }

    return res;
}

static bool handle_uh_call(CPUARMState *env, CPUState *cs);

void HELPER(pre_hvc)(CPUARMState *env)
{
    ARMCPU *cpu = env_archcpu(env);
    CPUState *cs = CPU(cpu);
    int cur_el = arm_current_el(env);
    /* FIXME: Use actual secure state.  */
    bool secure = false;
    bool undef;

    /* First check for Samsung UH (RKP/KDP) calls that need emulation */
    if (handle_uh_call(env, cs)) {
        return;
    }

    if (arm_is_psci_call(cpu, EXCP_HVC)) {
        /* If PSCI is enabled and this looks like a valid PSCI call then
         * that overrides the architecturally mandated HVC behaviour.
         */
        return;
    }

    if (!arm_feature(env, ARM_FEATURE_EL2)) {
        /* If EL2 doesn't exist, HVC always UNDEFs */
        undef = true;
    } else if (arm_feature(env, ARM_FEATURE_EL3)) {
        /* EL3.HCE has priority over EL2.HCD. */
        undef = !(env->cp15.scr_el3 & SCR_HCE);
    } else {
        undef = env->cp15.hcr_el2 & HCR_HCD;
    }

    /* In ARMv7 and ARMv8/AArch32, HVC is undef in secure state.
     * For ARMv8/AArch64, HVC is allowed in EL3.
     * Note that we've already trapped HVC from EL0 at translation
     * time.
     */
    if (secure && (!is_a64(env) || cur_el == 1)) {
        undef = true;
    }

    if (undef) {
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }
}

/*
 * Samsung UH (RKP/KDP) hypervisor emulation.
 *
 * The Samsung kernel's uh_call() issues SMC #0 to talk to the Samsung
 * hypervisor: RKP manages a read-only page pool, while KDP (Kernel Data
 * Protection) keeps selected structures (creds, vfsmounts, slab
 * free-pointers of protected caches) writable only by the hypervisor.
 * There is no EL3 firmware in this setup, so the calls are emulated here.
 *
 * KDP makes the kernel delegate every write to protected structures to
 * the hypervisor, and the kernel verifies the results (the panic checks
 * in prepare_ro_creds() and security_integrity_current()), so the writes
 * must be performed faithfully.  Structure layouts are not hardcoded:
 * KDP_INIT and NS_INIT hand us every field offset we need.
 *
 * All guest memory accesses go through the guest's own page tables via
 * cpu_memory_rw_debug(), which handles linear-map, kernel-image and
 * vmalloc addresses with any physvirt/KASLR configuration.  The static
 * offsets below are only a fallback for early-boot calls made before
 * the linear map is fully set up:
 *   kernel image: VA 0xffffffc008000000 -> PA 0xa8000000
 *   linear map:   VA 0xffffff8000000000 -> PA 0x40000000 (QEMU virt DRAM)
 */
#define UH_APP_RKP              0xc300c001
#define UH_APP_KDP              0xc300c002

/* RKP commands */
#define RKP_GET_RO_INFO         0x02
#define RKP_ROBUFFER_ALLOC      0x07
#define RKP_ROBUFFER_FREE       0x08

/* KDP commands (include/linux/kdp.h) */
#define KDP_INIT                0x00
#define KDP_JARRO_TSEC_SIZE     0x02
#define KDP_SET_SLAB_RO         0x03
#define KDP_SET_FREEPTR         0x04
#define KDP_PREPARE_RO_CRED     0x05
#define KDP_SET_CRED_PGD        0x06
#define KDP_SELINUX_CRED_FREE   0x07
#define KDP_PGD_RWX             0x08
#define KDP_MARK_PPT            0x09
#define KDP_NS_INIT             0x10
#define KDP_SET_NS_BP           0x11
#define KDP_SET_NS_DATA         0x12
#define KDP_SET_NS_ROOT_SB      0x13
#define KDP_SET_NS_SB_VFSMOUNT  0x14
#define KDP_SET_NS_FLAGS        0x15

#define KDP_CMD_COPY_CREDS      0

#define KIMAGE_VADDR            0xffffffc000000000ULL
#define KIMAGE_VOFFSET          0xffffffbf60000000ULL
#define LINEAR_MAP_VADDR        0xffffff8000000000ULL
#define LINEAR_MAP_VOFFSET      0xffffff7f80000000ULL

/*
 * RKP robuffer pool.  On real hardware the hypervisor owns a reserved
 * DDR carveout and recycles it; the kernel (pgd.c/pgd_alloc, mmu.c,
 * slub.c) both allocates AND frees robuffer pages.  The previous model
 * (a plain downward bump allocator with FREE stubbed out) had three
 * fatal bugs: the range was never reserved from the guest's buddy
 * allocator (double allocation), frees never recycled, and the pool
 * walked off System RAM into the Qualcomm carveout holes — handing the
 * kernel pgtable pages that are not in the linear map (crash at
 * ffffff8026fffXXX, pool page 0xa6fff000 = allocation #3841).
 *
 * Now: a bounded bitmap allocator over [RKP_POOL_BASE, RKP_POOL_TOP).
 * The matching reserved-memory DTB node (virt.c) keeps these pages out
 * of the buddy allocator while staying linear-mapped, exactly like the
 * device's real carveout.  Exhaustion returns 0 — the kernel treats
 * rkp_ro_alloc() failure as ENOMEM instead of eating a wild page.
 */
#define RKP_POOL_BASE           0xa7000000ULL
#define RKP_POOL_TOP            0xa7f00000ULL
#define RKP_POOL_PAGES          ((RKP_POOL_TOP - RKP_POOL_BASE) >> 12)
#define RKP_POOL_MAP_BYTES      ((RKP_POOL_PAGES + 7) / 8)

static uint8_t rkp_pool_map[RKP_POOL_MAP_BYTES];
static bool rkp_pool_inited;

static void rkp_pool_init_once(void)
{
    if (!rkp_pool_inited) {
        memset(rkp_pool_map, 0xff, sizeof(rkp_pool_map)); /* 1 = free */
        rkp_pool_inited = true;
    }
}

static int rkp_pool_bit_is_free(uint64_t idx)
{
    return (rkp_pool_map[idx >> 3] >> (idx & 7)) & 1;
}

static void rkp_pool_bit_set(uint64_t idx, bool free)
{
    if (free) {
        rkp_pool_map[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    } else {
        rkp_pool_map[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
    }
}

/* Allocate nr contiguous pages; returns base PA or 0 on exhaustion. */
static uint64_t rkp_pool_alloc(uint64_t nr)
{
    uint64_t run = 0, start = 0;
    static const uint8_t zero_page[0x1000];

    rkp_pool_init_once();
    for (uint64_t i = 0; i < RKP_POOL_PAGES; i++) {
        if (rkp_pool_bit_is_free(i)) {
            if (run == 0) {
                start = i;
            }
            if (++run == nr) {
                for (uint64_t j = start; j < start + nr; j++) {
                    rkp_pool_bit_set(j, false);
                }
                /*
                 * The kernel trusts the hypervisor to hand ZEROED pages
                 * (pgd_alloc()/rkp_ro_alloc() does no memset — b0q has no
                 * pgd_ctor and bypasses __GFP_ZERO).  Fresh QEMU RAM is
                 * zero, but recycled pool pages still hold stale pgtable
                 * entries → wild walks (copy_page_range crash).  Zero on
                 * every alloc, like the real ro-buffer allocator.
                 */
                for (uint64_t j = 0; j < nr; j++) {
                    address_space_write(&address_space_memory,
                                        RKP_POOL_BASE + (start + j) * 0x1000,
                                        MEMTXATTRS_UNSPECIFIED,
                                        zero_page, sizeof(zero_page));
                }
                return RKP_POOL_BASE + start * 0x1000;
            }
        } else {
            run = 0;
        }
    }
    fprintf(stderr, "[uh] RKP pool EXHAUSTED (%llu pages requested)\n",
            (unsigned long long)nr);
    return 0;
}

static void rkp_pool_free_pa(uint64_t pa)
{
    if (pa < RKP_POOL_BASE || pa >= RKP_POOL_TOP || (pa & 0xfff)) {
        return;
    }
    rkp_pool_bit_set((pa - RKP_POOL_BASE) >> 12, true);
}

/* Field offsets handed over by KDP_INIT (struct kdp_init) */
static struct {
    bool     valid;
    uint64_t init_mm_pgd;    /* swapper_pg_dir */
    uint32_t cred_size;      /* sizeof(struct cred_kdp) */
    uint32_t pgd_mm;         /* offsetof(struct mm_struct, pgd) */
    uint32_t bp_pgd_cred;    /* offsetof(struct cred_kdp, bp_pgd) */
    uint32_t bp_task_cred;   /* offsetof(struct cred_kdp, bp_task) */
    uint32_t security_cred;  /* offsetof(struct cred, security) */
    uint32_t usage_cred;     /* offsetof(struct cred_kdp, use_cnt) */
    uint32_t cred_task;      /* offsetof(struct task_struct, cred) */
    uint32_t mm_task;        /* offsetof(struct task_struct, mm) */
    uint32_t bp_cred_secptr; /* offsetof(struct task_security_struct, bp_cred) */
} kdp_par;

/* Field offsets handed over by NS_INIT (struct ns_param) */
static struct {
    bool     valid;
    uint32_t bp_offset;      /* offsetof(struct kdp_vfsmount, bp_mount) */
    uint32_t sb_offset;      /* offsetof(struct vfsmount, mnt_sb) */
    uint32_t flag_offset;    /* offsetof(struct vfsmount, mnt_flags) */
} ns_par = {
    /* confirmed on this kernel even before NS_INIT arrives */
    .bp_offset = 56, .sb_offset = 8, .flag_offset = 16,
};

static bool uh_guest_read(CPUState *cs, uint64_t va, void *buf, size_t len)
{
    return cpu_memory_rw_debug(cs, va, buf, len, false) == 0;
}

static bool uh_guest_write(CPUState *cs, uint64_t va, const void *buf, size_t len)
{
    uint64_t pa;

    if (cpu_memory_rw_debug(cs, va, (void *)buf, len, true) == 0) {
        return true;
    }
    /*
     * Early-boot fallback for addresses not reachable through the guest
     * page tables yet (host and guest are both little-endian).
     */
    if (va >= KIMAGE_VADDR) {
        pa = va - KIMAGE_VOFFSET;
    } else if (va >= LINEAR_MAP_VADDR) {
        pa = va - LINEAR_MAP_VOFFSET;
    } else {
        fprintf(stderr, "[uh] write to unmapped guest VA 0x%016" PRIx64
                " failed\n", va);
        return false;
    }
    return address_space_write(&address_space_memory, pa,
                               MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static bool uh_write_u64(CPUState *cs, uint64_t va, uint64_t val)
{
    return uh_guest_write(cs, va, &val, sizeof(val));
}

static void kdp_handle_init(CPUState *cs, uint64_t par_va)
{
    /* struct kdp_init { u64 x3; u32 x20; u64; struct { u64 x2; }; } */
    uint8_t buf[100];

    if (!uh_guest_read(cs, par_va, buf, sizeof(buf))) {
        fprintf(stderr, "[uh] KDP_INIT: cannot read params @0x%016" PRIx64
                "\n", par_va);
        return;
    }
    kdp_par.init_mm_pgd    = ldq_le_p(buf + 16);
    kdp_par.cred_size      = ldl_le_p(buf + 24);
    kdp_par.pgd_mm         = ldl_le_p(buf + 32);
    kdp_par.bp_pgd_cred    = ldl_le_p(buf + 52);
    kdp_par.bp_task_cred   = ldl_le_p(buf + 56);
    kdp_par.security_cred  = ldl_le_p(buf + 64);
    kdp_par.usage_cred     = ldl_le_p(buf + 68);
    kdp_par.cred_task      = ldl_le_p(buf + 72);
    kdp_par.mm_task        = ldl_le_p(buf + 76);
    kdp_par.bp_cred_secptr = ldl_le_p(buf + 92);
    kdp_par.valid = kdp_par.cred_size > 0 && kdp_par.cred_size <= 4096;
    fprintf(stderr, "[uh] KDP_INIT: cred_size=%u pgd_mm=%u bp_pgd=%u "
            "bp_task=%u security=%u use_cnt=%u cred_task=%u mm=%u bp_sec=%u\n",
            kdp_par.cred_size, kdp_par.pgd_mm, kdp_par.bp_pgd_cred,
            kdp_par.bp_task_cred, kdp_par.security_cred, kdp_par.usage_cred,
            kdp_par.cred_task, kdp_par.mm_task, kdp_par.bp_cred_secptr);
}

static void kdp_handle_ns_init(CPUState *cs, uint64_t par_va)
{
    /* struct ns_param { u32 x6 }; data_offset is deprecated/unused */
    uint8_t buf[24];

    if (!uh_guest_read(cs, par_va, buf, sizeof(buf))) {
        fprintf(stderr, "[uh] NS_INIT: cannot read params @0x%016" PRIx64
                "\n", par_va);
        return;
    }
    ns_par.bp_offset   = ldl_le_p(buf + 8);
    ns_par.sb_offset   = ldl_le_p(buf + 12);
    ns_par.flag_offset = ldl_le_p(buf + 16);
    ns_par.valid = true;
    fprintf(stderr, "[uh] KDP NS_INIT: bp=%u sb=%u flags=%u\n",
            ns_par.bp_offset, ns_par.sb_offset, ns_par.flag_offset);
}/*
 * PREPARE_RO_CRED: the kernel allocated a cred_kdp from the (hypervisor
 * protected) cred_jar_ro cache plus a use-count cell and a security blob,
 * and hands us a stack image to install.  The kernel panics unless
 * bp_task, cred.security and use_cnt are set exactly as it expects, and
 * security_integrity_current() additionally requires bp_pgd to match the
 * task's mm and tsec->bp_cred to point back at the cred.
 */
static void kdp_handle_prepare_ro_cred(CPUState *cs, CPUARMState *env,
                                       uint64_t param_va, uint64_t current_val)
{
    /* struct cred_param { u64 cred, cred_ro, use_cnt_ptr, sec_ptr, type,
     *                     task_ptr/use_cnt }; */
    uint8_t  buf[48];
    uint64_t cred_va, cred_ro_va, use_cnt_va, sec_va, type, task_or_cnt;
    uint64_t current, mm, pgd;
    void    *img;

    if (!kdp_par.valid) {
        fprintf(stderr, "[uh] PREPARE_RO_CRED before valid KDP_INIT\n");
        return;
    }
    if (!uh_guest_read(cs, param_va, buf, sizeof(buf))) {
        fprintf(stderr, "[uh] PREPARE_RO_CRED: cannot read cred_param\n");
        return;
    }
    cred_va     = ldq_le_p(buf + 0);
    cred_ro_va  = ldq_le_p(buf + 8);
    use_cnt_va  = ldq_le_p(buf + 16);
    sec_va      = ldq_le_p(buf + 24);
    type        = ldq_le_p(buf + 32);
    task_or_cnt = ldq_le_p(buf + 40);

    /* Install the prepared cred image into the protected object. */
    img = g_malloc(kdp_par.cred_size);
    if (uh_guest_read(cs, cred_va, img, kdp_par.cred_size)) {
        uh_guest_write(cs, cred_ro_va, img, kdp_par.cred_size);
    }
    g_free(img);

    uh_write_u64(cs, cred_ro_va + kdp_par.security_cred, sec_va);
    uh_write_u64(cs, cred_ro_va + kdp_par.usage_cred, use_cnt_va);

    current = current_val;
    if (type == KDP_CMD_COPY_CREDS) {
        uh_write_u64(cs, cred_ro_va + kdp_par.bp_task_cred, task_or_cnt);
    } else {
        uh_write_u64(cs, cred_ro_va + kdp_par.bp_task_cred, current);
        pgd = kdp_par.init_mm_pgd;
        if (uh_guest_read(cs, current + kdp_par.mm_task, &mm, sizeof(mm))
            && mm) {
            uh_guest_read(cs, mm + kdp_par.pgd_mm, &pgd, sizeof(pgd));
        }
        uh_write_u64(cs, cred_ro_va + kdp_par.bp_pgd_cred, pgd);
    }

    /* tsec->bp_cred = cred_ro, checked by is_kdp_invalid_cred_sp() */
    uh_write_u64(cs, sec_va + kdp_par.bp_cred_secptr, cred_ro_va);
}

/* Handle Samsung KDP (Kernel Data Protection) uh_calls */
static bool handle_kdp_call(CPUARMState *env, CPUState *cs, uint64_t command,
                            uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3, uint64_t arg4)
{
    switch (command) {
    case KDP_INIT:
        kdp_handle_init(cs, arg0);
        break;

    case KDP_NS_INIT:
        kdp_handle_ns_init(cs, arg0);
        break;

    case KDP_SET_NS_BP:
        /* arg0=kdp_vfsmount VA, arg1=mount VA: vfsmount->bp_mount = mount */
        uh_write_u64(cs, arg0 + ns_par.bp_offset, arg1);
        break;

    case KDP_SET_NS_ROOT_SB:
        /* arg0=vfsmount VA, arg1=dentry VA, arg2=superblock VA; mnt_root is @0 */
        uh_write_u64(cs, arg0, arg1);
        uh_write_u64(cs, arg0 + ns_par.sb_offset, arg2);
        break;

    case KDP_SET_NS_FLAGS: {
        /* arg0=vfsmount VA, arg1=flags; mnt_flags is an int */
        uint32_t flags = (uint32_t)arg1;

        uh_guest_write(cs, arg0 + ns_par.flag_offset,
                       &flags, sizeof(flags));
        break;
    }

    case KDP_SET_FREEPTR:
        /* slub freepointer of protected caches: arg0=object, arg1=s->offset,
         * arg3=value to store at object+offset */
        uh_write_u64(cs, arg0 + arg1, arg3);
        break;

    case KDP_SELINUX_CRED_FREE:
        /* arg0=&cred->security: NULL it in the protected cred */
        uh_write_u64(cs, arg0, 0);
        break;

    case KDP_SET_CRED_PGD: {
        /* arg0=cred_kdp VA, arg1=pgd VA: cred_kdp->bp_pgd = pgd */
        uint64_t cur, cur_cred, real_cred, bp_task;

        if (!kdp_par.valid) {
            break;
        }
        uh_write_u64(cs, arg0 + kdp_par.bp_pgd_cred, arg1);
        /*
         * exec's SET_CRED_PGD passes only current->cred (subjective).
         * If an override cred is installed, real_cred would keep a stale
         * bp_pgd and cmp_sec_integrity() would flag it once revert_creds()
         * restores it.  Mirror the write into real_cred, but only when it
         * verifiably belongs to this same task.  current is in sp_el0.
         */
        cur = env->sp_el[0];
        if (!uh_guest_read(cs, cur + kdp_par.cred_task,
                           &cur_cred, sizeof(cur_cred)) ||
            cur_cred != arg0) {
            break;      /* call is for another task's cred (fork path) */
        }
        if (!uh_guest_read(cs, cur + kdp_par.cred_task - 8,
                           &real_cred, sizeof(real_cred)) ||
            real_cred == arg0 || real_cred < LINEAR_MAP_VADDR) {
            break;      /* no distinct real_cred, or implausible pointer */
        }
        if (!uh_guest_read(cs, real_cred + kdp_par.bp_task_cred,
                           &bp_task, sizeof(bp_task)) || bp_task != cur) {
            break;      /* not a KDP cred of this task */
        }
        uh_write_u64(cs, real_cred + kdp_par.bp_pgd_cred, arg1);
        break;
    }

    case KDP_PREPARE_RO_CRED:
        kdp_handle_prepare_ro_cred(cs, env, arg0, arg1);
        break;

    case KDP_JARRO_TSEC_SIZE:    /* informational only */
    case KDP_SET_SLAB_RO:        /* pages stay writable in our model */
    case KDP_PGD_RWX:            /* pages are already RWX */
    case KDP_MARK_PPT:           /* exec bookkeeping */
    case KDP_SET_NS_DATA:        /* mnt.data deprecated */
    case KDP_SET_NS_SB_VFSMOUNT: /* hypervisor-internal bookkeeping */
        break;

    default:
        fprintf(stderr, "[uh] unhandled KDP command 0x%" PRIx64 "\n",
                command);
        break;
    }

    env->xregs[0] = 0;
    env->pc += 4;
    cpu_loop_exit(cs);
    return true;
}

/* Handle Samsung RKP (Real-time Kernel Protection) uh_calls */
static bool handle_rkp_call(CPUARMState *env, CPUState *cs, uint64_t command,
                            uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    switch (command) {
    case RKP_ROBUFFER_ALLOC: {
        uint64_t base;
        uint64_t out_ptr_va = arg0;
        uint64_t nr_pages   = arg1;

        if (nr_pages == 0) {
            nr_pages = 1;
        }

        base = rkp_pool_alloc(nr_pages);
        for (uint64_t i = 0; i < nr_pages; i++) {
            uint64_t alloc_pa = base ? base + i * 0x1000 : 0;
            uh_write_u64(cs, out_ptr_va + i * 8, alloc_pa);
        }

        env->xregs[0] = 0;
        env->pc += 4;
        cpu_loop_exit(cs);
        return true;
    }

    case RKP_ROBUFFER_FREE: {
        /* arg0 = page VA as returned to the kernel (__phys_to_virt(pa)) */
        uint64_t va = arg0;

        if (va >= LINEAR_MAP_VADDR) {
            rkp_pool_free_pa(va - LINEAR_MAP_VOFFSET);
        } else {
            rkp_pool_free_pa(va);   /* tolerate a raw PA */
        }

        env->xregs[0] = 0;
        env->pc += 4;
        cpu_loop_exit(cs);
        return true;
    }

    case RKP_GET_RO_INFO:
        /* arg0 = &robuffer_base (PA), arg1 = &robuffer_size — kernel uses
         * phys_to_virt(base) for range checks (drivers/uh/rkp.c). */
        uh_write_u64(cs, arg0, RKP_POOL_BASE);
        uh_write_u64(cs, arg1, RKP_POOL_TOP - RKP_POOL_BASE);
        env->xregs[0] = 0;
        env->pc += 4;
        cpu_loop_exit(cs);
        return true;

    default:
        env->xregs[0] = 0;
        env->pc += 4;
        cpu_loop_exit(cs);
        return true;
    }
}

/* Unified handler for Samsung UH (RKP/KDP) SMC and HVC calls */
static bool handle_uh_call(CPUARMState *env, CPUState *cs)
{
    uint64_t x0 = env->xregs[0];
    uint64_t app_id;
    uint64_t command;
    uint64_t arg0, arg1, arg2, arg3, arg4;

    /* Format 1: Qualcomm SMC (x0 = 0xc300c001 / 0xc300c002) */
    if (x0 == 0xc300c001) {
        app_id = 1; /* UH_APP_RKP */
        command = env->xregs[1];
        arg0 = env->xregs[2];
        arg1 = env->xregs[3];
        arg2 = env->xregs[4];
        arg3 = env->xregs[5];
        arg4 = env->xregs[6];
    } else if (x0 == 0xc300c002) {
        app_id = 2; /* UH_APP_KDP */
        command = env->xregs[1];
        arg0 = env->xregs[2];
        arg1 = env->xregs[3];
        arg2 = env->xregs[4];
        arg3 = env->xregs[5];
        arg4 = env->xregs[6];
    }
    /* Format 2: Exynos / MediaTek / Generic uH (x0 = prefix | (appid << 8) | cmd) */
    else if ((x0 & 0xffff0000) == 0x83000000 || (x0 & 0xffff0000) == 0xc3000000) {
        app_id = (x0 >> 8) & 0xff;
        command = x0 & 0xff;
        arg0 = env->xregs[1];
        arg1 = env->xregs[2];
        arg2 = env->xregs[3];
        arg3 = env->xregs[4];
        arg4 = env->xregs[5];
    } else {
        return false;
    }

    switch (app_id) {
    case 1: /* UH_APP_RKP */
        return handle_rkp_call(env, cs, command, arg0, arg1, arg2);
    case 2: /* UH_APP_KDP */
        return handle_kdp_call(env, cs, command, arg0, arg1, arg2, arg3, arg4);
    default:
        /* Generic stub for other UH apps (PLATFORM=0, HARSH=5, HDM=6, etc.): return success */
        env->xregs[0] = 0;
        env->pc += 4;
        cpu_loop_exit(cs);
        return true;
    }
}

void HELPER(pre_smc)(CPUARMState *env, uint32_t syndrome)
{
    ARMCPU *cpu = env_archcpu(env);
    CPUState *cs = CPU(cpu);
    int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);
    bool smd_flag = env->cp15.scr_el3 & SCR_SMD;

    /* On ARMv8 with EL3 AArch64, SMD applies to both S and NS state. */
    bool smd = arm_feature(env, ARM_FEATURE_AARCH64) ? smd_flag
                                                      : smd_flag && !secure;

    if (!arm_feature(env, ARM_FEATURE_EL3) &&
        !(arm_hcr_el2_eff(env) & HCR_NV) &&
        cpu->psci_conduit != QEMU_PSCI_CONDUIT_SMC) {
        /*
         * No EL3 and not using PSCI-via-SMC: stub SMC.
         * First check for Samsung RKP calls that need emulation.
         */
        if (!handle_uh_call(env, cs)) {
            env->xregs[0] = 0;
            env->pc += 4;
            cpu_loop_exit(cs);
        }
        return;
    }

    if (cur_el == 1 && (arm_hcr_el2_eff(env) & HCR_TSC)) {
        raise_exception(env, EXCP_HYP_TRAP, syndrome, 2);
    }

    if (!arm_is_psci_call(cpu, EXCP_SMC) &&
        (smd || !arm_feature(env, ARM_FEATURE_EL3))) {
        if (arm_feature(env, ARM_FEATURE_EL3) &&
            !env->cp15.vbar_el[3]) {
            if (!handle_uh_call(env, cs)) {
                env->xregs[0] = 0;
                env->pc += 4;
                cpu_loop_exit(cs);
            }
            return;
        }
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }

    /*
     * If we get here, SMC is about to trap to EL3 with no firmware.
     */
    if (arm_feature(env, ARM_FEATURE_EL3) && !env->cp15.vbar_el[3]) {
        if (!handle_uh_call(env, cs)) {
            env->xregs[0] = 0;
            env->pc += 4;
            cpu_loop_exit(cs);
        }
    }
}

/* ??? Flag setting arithmetic is awkward because we need to do comparisons.
   The only way to do that in TCG is a conditional branch, which clobbers
   all our temporaries.  For now implement these as helper functions.  */

/* Similarly for variable shift instructions.  */

uint32_t HELPER(shl_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        if (shift == 32)
            env->CF = x & 1;
        else
            env->CF = 0;
        return 0;
    } else if (shift != 0) {
        env->CF = (x >> (32 - shift)) & 1;
        return x << shift;
    }
    return x;
}

uint32_t HELPER(shr_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        if (shift == 32)
            env->CF = (x >> 31) & 1;
        else
            env->CF = 0;
        return 0;
    } else if (shift != 0) {
        env->CF = (x >> (shift - 1)) & 1;
        return x >> shift;
    }
    return x;
}

uint32_t HELPER(sar_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        env->CF = (x >> 31) & 1;
        return (int32_t)x >> 31;
    } else if (shift != 0) {
        env->CF = (x >> (shift - 1)) & 1;
        return (int32_t)x >> shift;
    }
    return x;
}

uint32_t HELPER(ror_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift1, shift;
    shift1 = i & 0xff;
    shift = shift1 & 0x1f;
    if (shift == 0) {
        if (shift1 != 0)
            env->CF = (x >> 31) & 1;
        return x;
    } else {
        env->CF = (x >> (shift - 1)) & 1;
        return ((uint32_t)x >> shift) | (x << (32 - shift));
    }
}

void HELPER(probe_access)(CPUARMState *env, vaddr ptr,
                          uint32_t access_type, uint32_t mmu_idx,
                          uint32_t size)
{
    uint32_t in_page = -((uint32_t)ptr | TARGET_PAGE_SIZE);
    uintptr_t ra = GETPC();

    if (likely(size <= in_page)) {
        probe_access(env, ptr, size, access_type, mmu_idx, ra);
    } else {
        probe_access(env, ptr, in_page, access_type, mmu_idx, ra);
        probe_access(env, ptr + in_page, size - in_page,
                     access_type, mmu_idx, ra);
    }
}

/*
 * This function corresponds to AArch64.vESBOperation().
 * Note that the AArch32 version is not functionally different.
 */
void HELPER(vesb)(CPUARMState *env)
{
    /*
     * The EL2Enabled() check is done inside arm_hcr_el2_eff,
     * and will return HCR_EL2.VSE == 0, so nothing happens.
     */
    uint64_t hcr = arm_hcr_el2_eff(env);
    bool enabled = !(hcr & HCR_TGE) && (hcr & HCR_AMO);
    bool pending = enabled && (hcr & HCR_VSE);
    bool masked  = (env->daif & PSTATE_A);

    /* If VSE pending and masked, defer the exception.  */
    if (pending && masked) {
        uint32_t syndrome;

        if (arm_el_is_aa64(env, 1)) {
            /* Copy across IDS and ISS from VSESR. */
            syndrome = env->cp15.vsesr_el2 & 0x1ffffff;
        } else {
            ARMMMUFaultInfo fi = { .type = ARMFault_AsyncExternal };

            if (extended_addresses_enabled(env)) {
                syndrome = arm_fi_to_lfsc(&fi);
            } else {
                syndrome = arm_fi_to_sfsc(&fi);
            }
            /* Copy across AET and ExT from VSESR. */
            syndrome |= env->cp15.vsesr_el2 & 0xd000;
        }

        /* Set VDISR_EL2.A along with the syndrome. */
        env->cp15.vdisr_el2 = syndrome | (1u << 31);

        /* Clear pending virtual SError */
        env->cp15.hcr_el2 &= ~HCR_VSE;
        cpu_reset_interrupt(env_cpu(env), CPU_INTERRUPT_VSERR);
    }
}
