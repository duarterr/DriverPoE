/** @file hv9910_curve.c
 * @brief Implementation of the HV9910 brightness -> (LD, PWMD) mapping.
 * See hv9910_curve.h for the modes, the continuity proof, and the
 * resolution tables. Integer math, no state, no IDF dependencies -- the one
 * exception is a single powf() in the optional output-power linearization.
 */
#include "hv9910_curve.h"
#include <math.h>

/** @brief Q16 divide a/b with round-to-nearest; b == 0 -> full scale. */
static inline uint32_t q_div(uint32_t a, uint32_t b)
{
    if (b == 0) {
        return HV9910_Q_ONE;
    }
    return (uint32_t)((((uint64_t)a << HV9910_Q_BITS) + (b >> 1)) / b);
}

/*
 * Output-power linearization for the LD (analog current-reference) path.
 *
 * On this board the LD reference -> LED power transfer is far from linear
 * because the HV9910 buck changes conduction mode across the range. Measured
 * and normalised on an AUX/PoE+ supply (forward: power P vs drive x):
 *
 *   - DCM (x below ~0.97): the converter is discontinuous, average current
 *     is ~quadratic in the reference, so  P ~= (x/A)^2.5  with A = 0.833
 *     ("50%" -> ~16% power). Inverting:  x = A * P^0.4.
 *   - CCM (x near full):   the converter goes continuous, average current is
 *     ~linear in the reference minus a fixed ripple term, so
 *     P ~= (x - 0.875)/0.125. Inverting:  x = 0.875 + 0.125*P.
 *
 * The buck runs in whichever mode delivers the target power at the lower
 * drive, so the drive for a commanded level L is the minimum of the two
 * inverses (they cross at L ~= 0.78, x ~= 0.97):
 *
 *     drive(L) = min( 1.0758 * L^0.4 ,  0.875 + 0.125*L )      (1.0758 = 1/0.833)
 *
 * drive(0)=0, drive(1)=1, monotone. Round-trips to within ~1.5 pp of the
 * bench data across 0..100%. Applied to the curve's ld_duty_q when
 * linearization is enabled: PWM mode (ld_duty_q == Q_ONE, drive(1) == 1) is
 * untouched -- it has no analog path; ANALOG / HYBRID-above-knee get the
 * pre-distortion; HYBRID-below-knee freezes ld at drive(crossover) while
 * PWMD does the linear chop, so the knee stays continuous (both sides reduce
 * to light output ~ commanded level).
 */
#define LIN_DCM_GAIN  1.0758f   /* 1 / 0.833  -- DCM extrapolation reaches only 83.3% at full drive */
#define LIN_DCM_EXP   0.4f      /* DCM: P ~ drive^2.5  ->  drive ~ P^0.4 */
#define LIN_CCM_BASE  0.875f    /* CCM: drive ~ 0.875 + 0.125*P (fixed ripple offset) */
#define LIN_CCM_SLOPE 0.125f

static uint32_t linearize_ld(uint32_t l)
{
    if (l == 0u || l >= HV9910_Q_ONE) {
        return l;   /* drive(0)==0 and drive(1)==1 anyway */
    }
    float p = (float)l / (float)HV9910_Q_ONE;
    float dcm = LIN_DCM_GAIN * powf(p, LIN_DCM_EXP);
    float ccm = LIN_CCM_BASE + LIN_CCM_SLOPE * p;
    float drive = (dcm < ccm) ? dcm : ccm;
    if (drive < 0.0f) {
        drive = 0.0f;
    }
    if (drive > 1.0f) {
        drive = 1.0f;
    }
    return (uint32_t)(drive * (float)HV9910_Q_ONE + 0.5f);
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
                                     uint32_t crossover_q, uint32_t min_on_frac_q,
                                     bool linearize)
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

    /* Pre-distort the LD reference so measured power tracks `level_q`. No-op
     * for PWM (ld_duty_q == Q_ONE). */
    if (linearize) {
        o.ld_duty_q = linearize_ld(o.ld_duty_q);
    }

    return o;
}
