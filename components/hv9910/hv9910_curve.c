/** @file hv9910_curve.c
 * @brief Implementation of the HV9910 brightness -> (LD, PWMD) mapping.
 * See hv9910_curve.h for the modes, the continuity proof, and the
 * resolution tables. Pure integer math, no state, no IDF dependencies.
 */
#include "hv9910_curve.h"

/** @brief Q16 divide a/b with round-to-nearest; b == 0 -> full scale. */
static inline uint32_t q_div(uint32_t a, uint32_t b)
{
    if (b == 0) {
        return HV9910_Q_ONE;
    }
    return (uint32_t)((((uint64_t)a << HV9910_Q_BITS) + (b >> 1)) / b);
}

/**
 * @brief Applies the min-on-time rule to a requested PWMD duty: the burst
 * is either >= floor_q or exactly 0, never truncated.
 * @param d Requested duty, 0..Q_ONE.
 * @param floor_q Minimum non-zero duty, 0..Q_ONE (0 disables the rule).
 * @param lit Set to whether the result drives any light.
 * @return The allowed duty.
 */
static uint32_t snap_pwmd(uint32_t d, uint32_t floor_q, bool *lit)
{
    if (floor_q == 0) {
        *lit = (d > 0);
        return d;
    }
    if (d >= floor_q) {
        *lit = true;
        return d;
    }
    if (d >= (floor_q >> 1)) {
        *lit = true;
        return floor_q;   /* close enough -- round the request up to the floor */
    }
    *lit = false;
    return 0;             /* too small for a clean burst -- go dark */
}

hv9910_curve_out_t hv9910_curve_eval(uint8_t mode, uint32_t level_q,
                                     uint32_t crossover_q, uint32_t min_on_frac_q)
{
    hv9910_curve_out_t o = { 0u, 0u, false };

    if (level_q > HV9910_Q_ONE) {
        level_q = HV9910_Q_ONE;
    }
    if (min_on_frac_q > HV9910_Q_ONE) {
        min_on_frac_q = HV9910_Q_ONE;
    }
    if (level_q == 0u) {
        return o;   /* every mode: 0 -> both duties 0, PWMD forced low by the caller */
    }

    switch (mode) {
    case HV9910_DIM_ANALOG:
        o.ld_duty_q   = level_q;
        o.pwmd_duty_q = HV9910_Q_ONE;   /* PWMD held continuously enabled */
        o.want_lit    = true;
        break;

    case HV9910_DIM_PWM: {
        bool lit;
        o.ld_duty_q   = HV9910_Q_ONE;   /* LD pinned at the max current reference */
        o.pwmd_duty_q = snap_pwmd(level_q, min_on_frac_q, &lit);
        o.want_lit    = lit;
        break;
    }

    case HV9910_DIM_HYBRID:
    default: {
        if (crossover_q == 0u) {
            crossover_q = 1u;
        }
        if (crossover_q > HV9910_Q_ONE) {
            crossover_q = HV9910_Q_ONE;
        }
        if (level_q >= crossover_q) {
            o.ld_duty_q   = level_q;             /* pure analog above the knee */
            o.pwmd_duty_q = HV9910_Q_ONE;
            o.want_lit    = true;
        } else {
            bool lit;
            o.ld_duty_q   = crossover_q;         /* LD frozen at the knee value */
            o.pwmd_duty_q = snap_pwmd(q_div(level_q, crossover_q), min_on_frac_q, &lit);
            o.want_lit    = lit;
        }
        break;
    }
    }

    return o;
}
