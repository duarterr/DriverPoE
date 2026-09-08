/** @file hv9910.c
 * @brief HV9910 LED driver control implementation.
 *
 * All hardware access and mutable state are owned exclusively by
 * hv9910_task; the public API functions only post commands to its queue.
 *
 * No NVS: every bit of state here is volatile. The luminaire always boots
 * with the LED off. The on-level comes from the network (DMX or admin);
 * a bare ON uses the last level commanded this boot, or 100% if none.
 *
 * Two LEDC outputs -- LD (GPIO -> RC/DAC -> HV9910 linear dimming) and
 * PWMD (logic line -> HV9910 digital dimming). hv9910_curve.c maps a
 * commanded brightness to both duties per the selected mode; PWMD at 0 is
 * the real "off". No fade engine: every change is applied at once.
 */
#include "hv9910.h"
#include "hv9910_curve.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "HV9910";

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LD_TIMER         LEDC_TIMER_0
#define LD_CHANNEL       LEDC_CHANNEL_0
#define PWMD_TIMER       LEDC_TIMER_1
#define PWMD_CHANNEL     LEDC_CHANNEL_1

#define DEFAULT_ON_PERCENT   100  /**< Level for a bare ON when nothing has set one yet. */

#define TASK_STACK_SIZE             3072
#define TASK_PRIORITY               (tskIDLE_PRIORITY + 6)
#define CMD_QUEUE_LEN                8
#define CMD_QUEUE_SEND_TIMEOUT_MS   100

#define IDENTIFY_BLINK_CYCLES     6
#define IDENTIFY_BLINK_PERIOD_MS  400

static hv9910_config_t s_config;

/* --- runtime dimming config (from driver_config; volatile) --------------- */
static hv9910_dimming_t s_dimming;
static uint32_t s_crossover_q   = HV9910_Q_ONE / 5;   /* derived from crossover_pct */
static uint32_t s_min_on_frac_q = 0;                  /* derived from min_on_time_us x pwm_freq_hz */
static bool     s_lin_enable    = true;               /* linearize the LD (analog) output-power transfer */
static uint32_t s_ld_full   = 1u << 10;               /* LEDC duty for 100% on the LD timer */
static uint32_t s_pwmd_full = 1u << 10;               /* LEDC duty for 100% on the PWMD timer */
static uint32_t s_output_scale_q = HV9910_Q_ONE;      /* power_manager cap, applied to the commanded level; Q_ONE = no cap */

/* --- driver state ------------------------------------------------------- */
static volatile bool s_enabled = false;              /* LED currently lit */
static volatile uint8_t s_dim_percent = 0;           /* last rendered level, 0..100 */
static uint32_t s_level_q = 0;                        /* last rendered level, Q16 (authoritative) */
static volatile uint8_t s_last_nonzero_percent = 0;  /* 0 = no level set this boot */
static volatile bool s_desired_on = false;           /* network's last on/off desire; survives a power blip */
static volatile bool s_power_cut = false;            /* set by EMERGENCY_OFF; blocks re-lighting until an
                                                        explicit ENABLE clears it */
static bool s_ledc_started = false;

/** @brief Command types accepted by hv9910_task. */
typedef enum {
    HV_CMD_ENABLE,         /**< Clear the power-cut latch, set s_desired_on, apply cmd.percent. */
    HV_CMD_DISABLE,
    HV_CMD_SET_DIM,        /**< Set the level (PWMD follows it: 0 -> off, >0 -> lit). */
    HV_CMD_IDENTIFY,
    HV_CMD_SET_PENDING,    /**< Record cmd.on (+ optionally cmd.percent as the last level); no hardware change. */
    HV_CMD_EMERGENCY_OFF,  /**< Force PWMD to 0 now, latch it; leave s_desired_on untouched. */
    HV_CMD_APPLY_DIMMING,  /**< Adopt cmd.dim: reconfigure LEDC, re-render the current level. */
    HV_CMD_SET_SCALE,      /**< Set the global LD-reference cap (cmd.percent), re-render. */
} hv9910_cmd_type_t;

/** @brief One queued command for hv9910_task. */
typedef struct {
    hv9910_cmd_type_t type; /**< Command type. */
    uint8_t percent;        /**< Brightness, for ENABLE/SET_DIM; level to remember for SET_PENDING. */
    bool on;                /**< Desired state, for SET_PENDING. */
    hv9910_dimming_t dim;   /**< Valid only for APPLY_DIMMING. */
} hv9910_cmd_t;

static QueueHandle_t s_cmd_queue;
static bool s_task_ready = false;

/** @brief Deferred/multi-step action hv9910_task is waiting to fire. */
typedef enum {
    PENDING_NONE,
    PENDING_IDENTIFY_STEP, /**< Advance to the next step of an in-progress IDENTIFY blink. */
} pending_action_t;

static volatile pending_action_t s_pending = PENDING_NONE;
static TickType_t s_pending_deadline;

static int s_identify_steps_left;
static bool s_identify_next_is_on;
static bool s_identify_saved_enabled;
static bool s_identify_saved_desired;
static uint32_t s_identify_saved_level_q;

/**
 * @brief Recomputes the Q16 values derived from s_dimming.
 * @return None.
 */
static void recompute_derived(void)
{
    s_crossover_q = ((uint32_t)s_dimming.crossover_pct * HV9910_Q_ONE) / 100u;

    uint64_t frac = ((uint64_t)s_dimming.min_on_time_us * s_dimming.pwm_freq_hz * HV9910_Q_ONE) / 1000000ULL;
    s_min_on_frac_q = (frac > HV9910_Q_ONE) ? HV9910_Q_ONE : (uint32_t)frac;

    s_lin_enable = (s_dimming.lin_enable != 0);
}

/**
 * @brief Picks the LEDC duty resolution (bits) for a PWM frequency, so
 * that freq * 2^bits stays within the 80 MHz APB limit.
 * @param freq_hz PWM frequency.
 * @return Duty resolution in bits (4..14).
 */
static uint32_t pick_duty_res_bits(uint32_t freq_hz)
{
    if (freq_hz == 0) {
        freq_hz = 1;
    }
    uint32_t ratio = 80000000u / freq_hz;   /* f_APB / f */
    uint32_t bits = 4;
    while (bits < 14 && (1u << (bits + 1)) <= ratio) {
        bits++;
    }
    return bits;
}

/**
 * @brief Scales a Q16 fraction (0..HV9910_Q_ONE) to an LEDC duty
 * (0..full), with round-to-nearest. full == HV9910_Q_ONE maps to `full`,
 * which the LEDC treats as constant-on.
 * @param q Fraction.
 * @param full LEDC duty for 100%.
 * @return LEDC duty.
 */
static uint32_t scale_duty(uint32_t q, uint32_t full)
{
    return (uint32_t)(((uint64_t)q * full + (HV9910_Q_ONE >> 1)) >> HV9910_Q_BITS);
}

/**
 * @brief Multiplies two Q16 fractions (round-to-nearest).
 * @param a First fraction, 0..HV9910_Q_ONE.
 * @param b Second fraction, 0..HV9910_Q_ONE.
 * @return a*b in Q16.
 */
static uint32_t mul_q(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b + (HV9910_Q_ONE >> 1)) >> HV9910_Q_BITS);
}

/**
 * @brief (Re)configures both LEDC timers for the current s_dimming
 * frequencies and refreshes s_ld_full / s_pwmd_full.
 * @return None.
 */
static void reconfigure_ledc(void)
{
    uint32_t ld_bits   = pick_duty_res_bits(s_dimming.analog_freq_hz);
    uint32_t pwmd_bits = pick_duty_res_bits(s_dimming.pwm_freq_hz);

    ledc_timer_config_t ld_t = {
        .speed_mode = LEDC_MODE,
        .timer_num = LD_TIMER,
        .duty_resolution = (ledc_timer_bit_t)ld_bits,
        .freq_hz = s_dimming.analog_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ld_t));

    ledc_timer_config_t pwmd_t = {
        .speed_mode = LEDC_MODE,
        .timer_num = PWMD_TIMER,
        .duty_resolution = (ledc_timer_bit_t)pwmd_bits,
        .freq_hz = s_dimming.pwm_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&pwmd_t));

    s_ld_full   = 1u << ld_bits;
    s_pwmd_full = 1u << pwmd_bits;
}

/**
 * @brief Configures the LEDC timers and both channels, once.
 * @return None.
 */
static void ledc_start_if_needed(void)
{
    if (s_ledc_started) {
        return;
    }

    reconfigure_ledc();

    ledc_channel_config_t ld_ch = {
        .gpio_num = s_config.ld_pin,
        .speed_mode = LEDC_MODE,
        .channel = LD_CHANNEL,
        .timer_sel = LD_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = s_config.ld_invert ? 1 : 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ld_ch));

    ledc_channel_config_t pwmd_ch = {
        .gpio_num = s_config.pwmd_pin,
        .speed_mode = LEDC_MODE,
        .channel = PWMD_CHANNEL,
        .timer_sel = PWMD_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = s_config.pwmd_invert ? 1 : 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&pwmd_ch));

    s_ledc_started = true;
}

/**
 * @brief Renders one brightness level to both channels + the lit/dark
 * decision. Instantaneous; the single place hardware duties are written.
 * @param level_q Commanded brightness, Q16.
 * @return None.
 */
static void apply_level(uint32_t level_q)
{
    ledc_start_if_needed();

    /* s_output_scale_q (the power_manager Type-1 cap) is applied to the
     * *commanded* level, before the curve. With linearization on, the curve
     * output is proportional to its input in physical units, so a 0.51 cap
     * yields 0.51 of max power; PWMD keeps its full range either way. */
    uint32_t eff_q = mul_q(level_q, s_output_scale_q);

    hv9910_curve_out_t o = hv9910_curve_eval(s_dimming.mode, eff_q, s_crossover_q,
                                             s_min_on_frac_q, s_lin_enable);
    bool lit = o.want_lit && !s_power_cut;

    uint32_t ld_duty   = lit ? scale_duty(o.ld_duty_q,   s_ld_full)   : 0;
    uint32_t pwmd_duty = lit ? scale_duty(o.pwmd_duty_q, s_pwmd_full) : 0;

    s_level_q = level_q;
    s_dim_percent = (uint8_t)((level_q * 100u + (HV9910_Q_ONE / 2)) / HV9910_Q_ONE);

    ledc_set_duty(LEDC_MODE, LD_CHANNEL, ld_duty);
    ledc_update_duty(LEDC_MODE, LD_CHANNEL);
    ledc_set_duty(LEDC_MODE, PWMD_CHANNEL, pwmd_duty);
    ledc_update_duty(LEDC_MODE, PWMD_CHANNEL);

    if (lit && !s_enabled) {
        s_enabled = true;
        ESP_LOGI(TAG, "Driver ON (%u%%, mode %u)", (unsigned)s_dim_percent, (unsigned)s_dimming.mode);
    } else if (!lit && s_enabled) {
        s_enabled = false;
        ESP_LOGI(TAG, "Driver OFF (PWMD 0)");
    }
}

/**
 * @brief Clamps a percent and converts it to Q16.
 * @param percent 0-100 (clamped).
 * @return Level in Q16.
 */
static uint32_t pct_to_q(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return ((uint32_t)percent * HV9910_Q_ONE) / 100u;
}

/**
 * @brief Sets the brightness (never touches NVS).
 * @param percent Brightness, 0-100.
 * @param remember true to record a nonzero level as the volatile "last level".
 * @return None.
 */
static void hv_do_set_dim(uint8_t percent, bool remember)
{
    if (percent > 100) {
        percent = 100;
    }
    if (remember && percent > 0) {
        s_last_nonzero_percent = percent;
    }
    apply_level(pct_to_q(percent));
}

/**
 * @brief Marks the driver network-desired-on, clears the power-cut latch,
 * and applies an explicit brightness.
 * @param percent Target brightness, 0-100.
 * @return None.
 */
static void hv_do_enable(uint8_t percent)
{
    s_desired_on = true;
    s_power_cut = false;
    hv_do_set_dim(percent, true);
}

/**
 * @brief Turns the driver off (PWMD to 0).
 * @return None.
 */
static void hv_do_disable(void)
{
    s_desired_on = false;
    apply_level(0);
}

/**
 * @brief Adopts a new dimming configuration: reconfigures the LEDC timers
 * and re-renders the current level under the new curve.
 * @param p New dimming config.
 * @return None.
 */
static void hv_do_apply_dimming(const hv9910_dimming_t *p)
{
    s_dimming = *p;
    recompute_derived();
    ledc_start_if_needed();
    reconfigure_ledc();
    apply_level(s_level_q);
    ESP_LOGI(TAG, "Dimming: mode=%u pwm=%uHz analog=%uHz min_on=%uus xover=%u%% lin=%s",
             (unsigned)s_dimming.mode, (unsigned)s_dimming.pwm_freq_hz,
             (unsigned)s_dimming.analog_freq_hz, (unsigned)s_dimming.min_on_time_us,
             (unsigned)s_dimming.crossover_pct, s_dimming.lin_enable ? "on" : "off");
}

/**
 * @brief Sets the global output power cap and re-renders the current level.
 * @param percent Cap, 1-100.
 * @return None.
 */
static void hv_do_set_scale(uint8_t percent)
{
    if (percent < 1) {
        percent = 1;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_output_scale_q = ((uint32_t)percent * HV9910_Q_ONE) / 100u;
    apply_level(s_level_q);
    ESP_LOGI(TAG, "Output power capped to %u%% of max", (unsigned)percent);
}

/**
 * @brief Advances the in-progress IDENTIFY sequence by one step, or ends it.
 * @return None.
 */
static void do_identify_step(void)
{
    if (s_identify_steps_left <= 0) {
        /* Restore exactly what we found -- identify must not disturb the
         * on/off desire or the level. */
        apply_level(s_identify_saved_enabled ? s_identify_saved_level_q : 0);
        s_desired_on = s_identify_saved_desired;
        s_pending = PENDING_NONE;
        return;
    }

    apply_level(s_identify_next_is_on ? HV9910_Q_ONE : 0);
    s_identify_next_is_on = !s_identify_next_is_on;
    s_identify_steps_left--;

    s_pending = PENDING_IDENTIFY_STEP;
    s_pending_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDENTIFY_BLINK_PERIOD_MS);
}

/**
 * @brief Starts the IDENTIFY blink sequence.
 * @return None.
 */
static void hv_do_identify(void)
{
    s_identify_saved_enabled = s_enabled;
    s_identify_saved_level_q = s_level_q;
    s_identify_saved_desired = s_desired_on;
    s_power_cut = false;   /* IDENTIFY only runs once power is confirmed */

    s_identify_steps_left = IDENTIFY_BLINK_CYCLES * 2;
    s_identify_next_is_on = true;   /* first step lights it */
    do_identify_step();
}

/**
 * @brief Dispatches one command to its handler.
 * @param cmd Command to dispatch.
 * @return None.
 */
static void dispatch_cmd(const hv9910_cmd_t *cmd)
{
    switch (cmd->type) {
    case HV_CMD_ENABLE:
        hv_do_enable(cmd->percent);
        break;
    case HV_CMD_DISABLE:
        hv_do_disable();
        break;
    case HV_CMD_SET_DIM:
        hv_do_set_dim(cmd->percent, true);
        break;
    case HV_CMD_IDENTIFY:
        hv_do_identify();
        break;
    case HV_CMD_SET_PENDING:
        s_desired_on = cmd->on;
        if (cmd->percent > 0) {
            s_last_nonzero_percent = cmd->percent;
        }
        break;
    case HV_CMD_EMERGENCY_OFF:
        /* PWMD low is the real cut; latch it so a SET_DIM already queued
         * behind this can't re-light before an explicit ENABLE. */
        s_power_cut = true;
        s_pending = PENDING_NONE;
        if (s_ledc_started) {
            ledc_set_duty(LEDC_MODE, PWMD_CHANNEL, 0);
            ledc_update_duty(LEDC_MODE, PWMD_CHANNEL);
            ledc_set_duty(LEDC_MODE, LD_CHANNEL, 0);
            ledc_update_duty(LEDC_MODE, LD_CHANNEL);
        }
        s_enabled = false;
        s_dim_percent = 0;
        s_level_q = 0;
        ESP_LOGW(TAG, "Driver EMERGENCY OFF (PWMD forced low; resumes on power if it was on)");
        break;
    case HV_CMD_APPLY_DIMMING:
        hv_do_apply_dimming(&cmd->dim);
        break;
    case HV_CMD_SET_SCALE:
        hv_do_set_scale(cmd->percent);
        break;
    }
}

/**
 * @brief Fires whatever deferred action is currently pending.
 * @return None.
 */
static void fire_pending_action(void)
{
    switch (s_pending) {
    case PENDING_IDENTIFY_STEP:
        do_identify_step();
        break;
    case PENDING_NONE:
    default:
        break;
    }
}

/**
 * @brief Task that owns all HV9910 hardware access and serializes commands.
 * @param arg Unused.
 * @return Never returns.
 */
static void hv9910_task(void *arg)
{
    (void)arg;

    while (1) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (s_pending != PENDING_NONE) {
            TickType_t remaining = s_pending_deadline - xTaskGetTickCount();
            wait_ticks = ((int32_t)remaining > 0) ? remaining : 0;
        }

        hv9910_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, wait_ticks) == pdTRUE) {
            s_pending = PENDING_NONE;
            dispatch_cmd(&cmd);
        } else if (s_pending != PENDING_NONE) {
            fire_pending_action();
        }
    }
}

/**
 * @brief Posts a command to hv9910_task.
 * @param cmd Command to post.
 * @param to_front true to jump the queue (emergency cutoff only).
 * @return None.
 */
static void post_cmd(const hv9910_cmd_t *cmd, bool to_front)
{
    if (!s_task_ready) {
        ESP_LOGE(TAG, "hv9910 command queue not ready (init failed?) -- command type %d dropped", (int)cmd->type);
        return;
    }

    if (to_front) {
        if (xQueueSendToFront(s_cmd_queue, cmd, 0) == pdTRUE) {
            return;
        }
        hv9910_cmd_t discarded;
        xQueueReceive(s_cmd_queue, &discarded, 0);
        if (xQueueSendToFront(s_cmd_queue, cmd, 0) != pdTRUE) {
            ESP_LOGE(TAG, "hv9910 emergency command still couldn't be posted after making room -- should never happen");
        }
        return;
    }

    BaseType_t ok = xQueueSend(s_cmd_queue, cmd, pdMS_TO_TICKS(CMD_QUEUE_SEND_TIMEOUT_MS));
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "Failed to post hv9910 command (type %d, queue full?) -- dropped", (int)cmd->type);
    }
}

void hv9910_init(const hv9910_config_t *config)
{
    s_config = *config;

    /* Safe default until driver_config pushes the persisted config. */
    s_dimming.mode = HV9910_DIM_HYBRID;
    s_dimming.pwm_freq_hz = 2000;
    s_dimming.analog_freq_hz = 60000;
    s_dimming.min_on_time_us = 20;
    s_dimming.crossover_pct = 20;
    s_dimming.lin_enable = 1;
    recompute_derived();

    s_enabled = false;
    s_dim_percent = 0;
    s_level_q = 0;
    s_last_nonzero_percent = 0;
    s_desired_on = false;
    s_power_cut = false;
    s_ledc_started = false;
    s_output_scale_q = HV9910_Q_ONE;

    /* Both pins driven low before the LEDC takes them over: PWMD low =
     * driver disabled, so the LED is unambiguously off at boot. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_config.pwmd_pin) | (1ULL << s_config.ld_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    /* Drive each pin to the level that a duty of 0 produces once the LEDC
     * takes over (i.e. the "off" state), accounting for output_invert. */
    gpio_set_level(s_config.pwmd_pin, s_config.pwmd_invert ? 1 : 0);
    gpio_set_level(s_config.ld_pin, s_config.ld_invert ? 1 : 0);

    ESP_LOGI(TAG, "HV9910 initialized: PWMD off (LED off), default mode HYBRID");

    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(hv9910_cmd_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed -- HV9910 control disabled, driver stays off");
        return;
    }

    BaseType_t task_ok = xTaskCreate(hv9910_task, "hv9910", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed -- HV9910 control disabled, driver stays off");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return;
    }

    s_task_ready = true;
}

void hv9910_set_dimming(const hv9910_dimming_t *p)
{
    if (p == NULL) {
        return;
    }
    hv9910_cmd_t cmd = { .type = HV_CMD_APPLY_DIMMING, .dim = *p };
    post_cmd(&cmd, false);
}

void hv9910_set_output_scale(uint8_t percent)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_SET_SCALE, .percent = percent };
    post_cmd(&cmd, false);
}

uint8_t hv9910_get_output_scale_pct(void)
{
    return (uint8_t)((s_output_scale_q * 100u + (HV9910_Q_ONE / 2)) / HV9910_Q_ONE);
}

void hv9910_enable_at(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    hv9910_cmd_t cmd = { .type = HV_CMD_ENABLE, .percent = percent };
    post_cmd(&cmd, false);
}

void hv9910_enable(void)
{
    uint8_t target = (s_last_nonzero_percent > 0) ? s_last_nonzero_percent : DEFAULT_ON_PERCENT;
    hv9910_enable_at(target);
}

void hv9910_disable(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_DISABLE };
    post_cmd(&cmd, false);
}

void hv9910_emergency_disable(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_EMERGENCY_OFF };
    post_cmd(&cmd, true);
}

void hv9910_set_dim(uint8_t percent)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_SET_DIM, .percent = percent };
    post_cmd(&cmd, false);
}

void hv9910_identify(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_IDENTIFY };
    post_cmd(&cmd, false);
}

void hv9910_set_pending(bool on, uint8_t remember_pct)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_SET_PENDING, .on = on, .percent = remember_pct };
    post_cmd(&cmd, false);
}

uint8_t hv9910_get_dim_percent(void)
{
    return s_dim_percent;
}

uint8_t hv9910_get_last_nonzero_percent(void)
{
    return s_last_nonzero_percent;
}

bool hv9910_pending_on(void)
{
    return s_desired_on;
}

bool hv9910_is_enabled(void)
{
    return s_enabled;
}
