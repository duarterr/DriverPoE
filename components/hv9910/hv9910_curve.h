/** @file hv9910_curve.h
 * @brief Pure brightness -> (LD, PWMD) mapping for the HV9910's three
 * dimming modes. No IDF headers, no state, host-compilable.
 *
 * The HV9910 has two dimming inputs on this board:
 *   - LD   (linear dimming): an analog current reference, fed by a PWM
 *     through an RC low-pass on the PCB. Flicker-free, but the comparator
 *     offset / ~300 ns propagation delay dominate below ~10 % of nominal
 *     current, so it is neither accurate nor colour-stable down there, and
 *     it never reaches true zero.
 *   - PWMD (digital dimming): a logic line that gates the driver. Chopping
 *     it gives perfectly linear photometry and a stable CCT (the LED
 *     always runs at the same peak current), at the cost of bottom-end
 *     resolution (see the min-on-time note below). Driving it to 0 is the
 *     real "off".
 *
 * Modes:
 *   - HV9910_DIM_PWM    : chop PWMD, hold LD at max.
 *   - HV9910_DIM_ANALOG : modulate LD, hold PWMD continuously enabled.
 *   - HV9910_DIM_HYBRID : analog on LD down to `crossover`, then freeze LD
 *                         at the crossover value and take the rest out of
 *                         PWMD. Combines analog's freedom from flicker in
 *                         the top of the range with PWM's depth and
 *                         accuracy at the bottom.
 *
 * All levels/fractions are unsigned Q16 (HV9910_Q_ONE == 1.0).
 *
 * --- Output-power linearization (the `linearize` flag) ---
 * The LD reference -> LED power transfer on this board is far from linear:
 * the HV9910 buck is discontinuous over most of the range (power ~ drive^2.5
 * -- "50%" gives ~16% power) and goes continuous near full drive. When
 * `linearize` is set, hv9910_curve_eval pre-distorts the LD duty by the
 * measured inverse (see hv9910_curve.c for the derivation):
 *   drive(L) = min( 1.0758 * L^0.4 ,  0.875 + 0.125*L )
 * applied to ld_duty_q only, so PWM mode (ld_duty_q == Q_ONE) is untouched
 * -- it has no analog path -- and the HYBRID knee stays continuous (see
 * below): both branches still reduce to light output ~ commanded level, now
 * in *physical* units, not just in Q16 duty. Round-trips within ~1.5 pp.
 *
 * --- HYBRID continuity (why there is no visible step at the crossover) ---
 * Light output is proportional to  Phi = ld_duty_q * pwmd_duty_q / Q_ONE
 * (assuming LD -> current is linear, which holds above the ~10 % floor the
 * crossover is clamped to).
 *   level >= crossover:  Phi = level * 1            = level
 *   level <  crossover:  Phi = crossover * (level/crossover) = level
 * Both branches reduce to Phi(level) == level, so the two one-sided limits
 * at the crossover agree. q_div(C, C) rounds to exactly Q_ONE, so the knee
 * sample itself is exact; per-channel quantisation keeps |dPhi| within a
 * couple of LSB across the knee. Worked example, crossover C = 0x3333
 * (20 %), min_on_frac small:
 *   level = C - 1 : ld = C      = 0x3333, pwmd = q_div(C-1, C) ~= 0xFFFC -> Phi ~= 0x3332
 *   level = C     : ld = C      = 0x3333, pwmd = Q_ONE         = 0x10000 -> Phi  = 0x3333
 *   level = C + 1 : ld = C + 1  = 0x3334, pwmd = Q_ONE                   -> Phi  = 0x3334
 * The only intentional discontinuity is the min-on-time snap to zero, and
 * that lives near level 0 (level < crossover * min_on_frac), not at the knee.
 *
 * --- Min-on-time / bottom-of-scale resolution (PWM + HYBRID) ---
 * The HV9910 runs its own buck switching loop at ~120-170 kHz (period
 * ~6-8 us) -- far faster than our 1-5 kHz PWMD chop. Each PWMD conduction
 * burst must last a few of those internal cycles for the inductor current
 * to reach regime (~20 us default), or the LED gets a stunted, nonlinear
 * flash. So a requested PWMD duty is either >= min_on_frac_q or exactly 0
 * -- never a truncated burst.
 * min_on_frac_q = min_on_time_us * pwm_freq_hz / 1e6. Smallest non-zero
 * PWM step (and the width of the forced-dark zone just above 0):
 *
 *      pwm_freq   period    20 us floor   HYBRID floor (x crossover 0.20)
 *      1 kHz      1000 us    2.0 %         0.40 %
 *      2 kHz       500 us    4.0 %         0.80 %
 *      5 kHz       200 us   10.0 %         2.00 %
 *
 * Lower PWMD frequency -> finer bottom-end steps but more camera-beat risk;
 * higher -> the opposite. HYBRID dims deeper than pure PWM because the
 * crossover compresses PWMD's working range by the crossover fraction.
 *
 * --- ANALOG LEDC resolution vs frequency (informational) ---
 * The RC-fed PWM wants to be 40-80 kHz (ripple << 0.5 mV at the ~250 mV
 * full-scale CS reference, settling well under the actuation period). On
 * the ESP32 LEDC, freq * 2^bits <= 80 MHz (APB), so:
 *      40 kHz -> 10 bits (1024 steps)
 *      60 kHz -> 10 bits (1024 steps)   <- default
 *      80 kHz ->  9 bits ( 512 steps)   (2^10 * 80k = 81.9 MHz > 80 MHz)
 * This curve works in normalised Q16; hv9910.c scales the result to each
 * channel's live duty_max, so the resolution choice is transparent here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Q16 fixed-point: HV9910_Q_ONE represents 1.0 (full scale). */
#define HV9910_Q_BITS 16
#define HV9910_Q_ONE  (1u << HV9910_Q_BITS)

/** @brief Runtime-selectable dimming strategy. */
typedef enum {
    HV9910_DIM_PWM    = 0, /**< Chop PWMD; LD held at max. */
    HV9910_DIM_ANALOG = 1, /**< Modulate LD; PWMD held enabled. */
    HV9910_DIM_HYBRID = 2, /**< Analog above the crossover, PWM below. */
} hv9910_dim_mode_t;

/** @brief Result of evaluating the curve at one brightness level. */
typedef struct {
    uint32_t ld_duty_q;   /**< 0..Q_ONE -> LEDC channel driving LD. */
    uint32_t pwmd_duty_q; /**< 0..Q_ONE -> LEDC channel driving PWMD. */
    bool     want_lit;    /**< false: the caller should also force PWMD to 0 (driver off). */
} hv9910_curve_out_t;

/**
 * @brief Maps a commanded brightness to the two channel duties.
 * @param mode hv9910_dim_mode_t (out-of-range treated as HYBRID).
 * @param level_q Commanded brightness, 0..HV9910_Q_ONE (clamped).
 * @param crossover_q HYBRID knee, 0..HV9910_Q_ONE (clamped to [1, Q_ONE]).
 * @param min_on_frac_q PWMD burst floor as a fraction, 0..HV9910_Q_ONE
 *        (0 disables the snap).
 * @param linearize true to pre-distort the LD duty so LED power tracks
 *        level_q (see the linearization note above); no effect in PWM mode.
 * @return Duties and the lit/dark decision.
 */
hv9910_curve_out_t hv9910_curve_eval(uint8_t mode, uint32_t level_q,
                                     uint32_t crossover_q, uint32_t min_on_frac_q,
                                     bool linearize);

#ifdef __cplusplus
}
#endif
