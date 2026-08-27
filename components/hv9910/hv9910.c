/** @file hv9910.c
 * @brief HV9910 LED driver control implementation.
 *
 * All hardware access and mutable state are owned exclusively by
 * hv9910_task; the public API functions only post commands to its queue.
 *
 * No NVS: every bit of state here is volatile. The luminaire always boots
 * with the LED off. The on-level comes from the network (DMX or admin);
 * a bare ON uses the last level commanded this boot, or 100% if none.
 */
#include "hv9910.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>

static const char *TAG = "HV9910";

#define LEDC_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_DUTY_RES     LEDC_TIMER_10_BIT
#define LEDC_DUTY_MAX     ((1 << 10) - 1)

#define DEFAULT_ON_PERCENT   100  /**< Level for a bare ON when nothing has set one yet. */

#define TASK_STACK_SIZE             3072
#define TASK_PRIORITY               (tskIDLE_PRIORITY + 6)
#define CMD_QUEUE_LEN                8
#define CMD_QUEUE_SEND_TIMEOUT_MS   100

#define IDENTIFY_BLINK_CYCLES     6
#define IDENTIFY_BLINK_PERIOD_MS  400

static hv9910_config_t s_config;

/**
 * @brief GPIO level that asserts (disables) SHUTDOWN for the configured polarity.
 * @return GPIO level.
 */
static int shutdown_assert_level(void) { return s_config.shutdown_active_high ? 1 : 0; }

/**
 * @brief GPIO level that releases (enables) SHUTDOWN for the configured polarity.
 * @return GPIO level.
 */
static int shutdown_run_level(void)    { return s_config.shutdown_active_high ? 0 : 1; }

static volatile bool s_enabled = false;
static volatile uint8_t s_dim_percent = 0;
static volatile uint8_t s_last_nonzero_percent = 0;  /* 0 = no level set this boot */
static volatile bool s_desired_on = false;           /* network's last on/off desire; survives a power blip */
static bool s_ledc_started = false;

/** @brief Command types accepted by hv9910_task. */
typedef enum {
    HV_CMD_ENABLE,            /**< Release SHUTDOWN, ramp to cmd.percent. */
    HV_CMD_DISABLE,
    HV_CMD_SET_DIM,
    HV_CMD_IDENTIFY,
    HV_CMD_SET_PENDING,       /**< Record cmd.on as the desired state; no hardware change. */
    HV_CMD_EMERGENCY_OFF,     /**< Assert SHUTDOWN now; leave s_desired_on untouched. */
    HV_CMD_FADE_TO_ZERO_DONE, /**< Internal only, posted by hv_fade_end_cb() -- never sent by post_cmd() callers. */
} hv9910_cmd_type_t;

/** @brief One queued command for hv9910_task. */
typedef struct {
    hv9910_cmd_type_t type; /**< Command type. */
    uint32_t ramp_ms;       /**< Ramp duration, for ENABLE/DISABLE/SET_DIM. */
    uint8_t percent;        /**< Brightness, for ENABLE/SET_DIM. */
    bool on;                /**< Desired state, for SET_PENDING. */
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

/** @brief True while a fade-to-0 from hv_do_disable() is in flight; its completion
 * (hv_fade_end_cb) asserts SHUTDOWN. Cleared by any other dim/enable call so a
 * superseded fade can't fire a stale shutdown. */
static volatile bool s_awaiting_shutdown_fade = false;

static int s_identify_steps_left;
static bool s_identify_next_is_on;
static bool s_identify_saved_enabled;
static bool s_identify_saved_desired;
static uint8_t s_identify_saved_dim;

/**
 * @brief Writes the SHUTDOWN GPIO level.
 * @param level GPIO level to write.
 * @return None.
 */
static void shutdown_write(int level)
{
    gpio_set_level(s_config.shutdown_pin, level);
}

/**
 * @brief Clamps a requested ramp duration to the configured maximum.
 * @param ramp_ms Requested ramp duration in ms.
 * @return Clamped ramp duration in ms.
 */
static uint32_t clamp_ramp_ms(uint32_t ramp_ms)
{
    if (ramp_ms > s_config.max_ramp_ms) {
        ESP_LOGW(TAG, "ramp_ms=%" PRIu32 " exceeds the configured max (%" PRIu32 ") -- clamping",
                 ramp_ms, s_config.max_ramp_ms);
        return s_config.max_ramp_ms;
    }
    return ramp_ms;
}

/**
 * @brief LEDC fade-end ISR callback; posts HV_CMD_FADE_TO_ZERO_DONE if this
 * fade is the one hv_do_disable() is waiting on.
 * @param param Fade event info.
 * @param user_arg Unused.
 * @return true if a higher-priority task was woken.
 */
static bool IRAM_ATTR hv_fade_end_cb(const ledc_cb_param_t *param, void *user_arg)
{
    (void)user_arg;
    BaseType_t woken = pdFALSE;
    if (param->event == LEDC_FADE_END_EVT && s_awaiting_shutdown_fade) {
        s_awaiting_shutdown_fade = false;
        hv9910_cmd_t cmd = { .type = HV_CMD_FADE_TO_ZERO_DONE };
        xQueueSendFromISR(s_cmd_queue, &cmd, &woken);
    }
    return woken == pdTRUE;
}

/**
 * @brief Configures the LEDC timer/channel and installs the fade service, once.
 * @return None.
 */
static void ledc_start_if_needed(void)
{
    if (s_ledc_started) {
        return;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_SPEED_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = s_config.pwm_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = s_config.dimming_pin,
        .speed_mode = LEDC_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = s_config.dim_active_high ? 1 : 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    ledc_cbs_t callbacks = { .fade_cb = hv_fade_end_cb };
    ESP_ERROR_CHECK(ledc_cb_register(LEDC_SPEED_MODE, LEDC_CHANNEL, &callbacks, NULL));

    s_ledc_started = true;
}

/**
 * @brief Sets the LEDC duty cycle, with an optional hardware fade.
 * @param percent Brightness, 0-100.
 * @param ramp_ms Ramp duration in ms; 0 for an instant change.
 * @param remember true to record a nonzero level as the volatile "last
 * level" for a later bare enable; false for cosmetic changes (identify).
 * @return None.
 */
static void hv_do_set_dim(uint8_t percent, uint32_t ramp_ms, bool remember)
{
    if (percent > 100) {
        percent = 100;
    }
    s_dim_percent = percent;
    if (remember && percent > 0) {
        s_last_nonzero_percent = percent;
    }

    ledc_start_if_needed();

    uint32_t duty = ((uint32_t)percent * LEDC_DUTY_MAX) / 100;

    if (ramp_ms == 0) {
        s_awaiting_shutdown_fade = false;
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    } else {
        // Arms/disarms hv_fade_end_cb()'s shutdown intent for this fade.
        s_awaiting_shutdown_fade = (percent == 0);
        ledc_set_fade_with_time(LEDC_SPEED_MODE, LEDC_CHANNEL, duty, (int)ramp_ms);
        ledc_fade_start(LEDC_SPEED_MODE, LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
    }

    ESP_LOGI(TAG, "Dimming set to %u%% over %ums (duty=%u/%u)", percent, (unsigned)ramp_ms, (unsigned)duty, LEDC_DUTY_MAX);
}

/**
 * @brief Releases SHUTDOWN and ramps to an explicit brightness.
 * @param percent Target brightness, 0-100.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
static void hv_do_enable(uint8_t percent, uint32_t ramp_ms)
{
    s_desired_on = true;
    shutdown_write(shutdown_run_level());
    s_enabled = true;
    hv_do_set_dim(percent, ramp_ms, true);
    ESP_LOGW(TAG, "Driver ENABLED (SHUTDOWN released), ramping to %u%% over %ums", percent, (unsigned)ramp_ms);
}

/**
 * @brief Ramps the brightness to 0 and disables the driver.
 * @param ramp_ms Ramp duration in ms.
 * @return None.
 */
static void hv_do_disable(uint32_t ramp_ms)
{
    s_desired_on = false;
    hv_do_set_dim(0, ramp_ms, true);

    if (ramp_ms == 0) {
        shutdown_write(shutdown_assert_level());
        s_enabled = false;
        ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted)");
    }
    // else: SHUTDOWN fires later, from HV_CMD_FADE_TO_ZERO_DONE.
}

/**
 * @brief Advances the in-progress IDENTIFY sequence by one step, or ends it.
 * @return None.
 */
static void do_identify_step(void)
{
    if (s_identify_steps_left <= 0) {
        /* Restore exactly what we found -- identify must not disturb the
         * on/off desire or the remembered level. */
        if (s_identify_saved_enabled) {
            hv_do_set_dim(s_identify_saved_dim, 0, false);
        } else {
            shutdown_write(shutdown_assert_level());
            s_enabled = false;
            hv_do_set_dim(0, 0, false);
        }
        s_desired_on = s_identify_saved_desired;
        s_pending = PENDING_NONE;
        return;
    }

    hv_do_set_dim(s_identify_next_is_on ? 100 : 0, 0, false);
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
    s_identify_saved_dim = s_dim_percent;
    s_identify_saved_desired = s_desired_on;

    if (!s_enabled) {
        // Light the driver just for the blink -- no change to the on/off
        // desire or the remembered level.
        shutdown_write(shutdown_run_level());
        s_enabled = true;
    }

    s_identify_steps_left = IDENTIFY_BLINK_CYCLES * 2;
    s_identify_next_is_on = false;
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
        hv_do_enable(cmd->percent, cmd->ramp_ms);
        break;
    case HV_CMD_DISABLE:
        hv_do_disable(cmd->ramp_ms);
        break;
    case HV_CMD_SET_DIM:
        hv_do_set_dim(cmd->percent, cmd->ramp_ms, true);
        break;
    case HV_CMD_IDENTIFY:
        hv_do_identify();
        break;
    case HV_CMD_SET_PENDING:
        s_desired_on = cmd->on;
        break;
    case HV_CMD_EMERGENCY_OFF:
        // SHUTDOWN is the real cut; duty is irrelevant while it's asserted.
        s_awaiting_shutdown_fade = false;
        shutdown_write(shutdown_assert_level());
        s_enabled = false;
        s_dim_percent = 0;
        if (s_ledc_started) {
            ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
            ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
        }
        ESP_LOGW(TAG, "Driver EMERGENCY OFF (SHUTDOWN asserted; resumes on power if it was on)");
        break;
    case HV_CMD_FADE_TO_ZERO_DONE:
        // From hv_fade_end_cb(); s_enabled check is just idempotency.
        if (s_enabled) {
            shutdown_write(shutdown_assert_level());
            s_enabled = false;
            ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted, after ramp)");
        }
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

    gpio_config_t shutdown_cfg = {
        .pin_bit_mask = 1ULL << s_config.shutdown_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&shutdown_cfg));
    shutdown_write(shutdown_assert_level());

    gpio_config_t dim_cfg = {
        .pin_bit_mask = 1ULL << s_config.dimming_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dim_cfg));
    gpio_set_level(s_config.dimming_pin, s_config.dim_active_high ? 1 : 0);

    s_enabled = false;
    s_dim_percent = 0;
    s_last_nonzero_percent = 0;
    s_desired_on = false;
    s_ledc_started = false;

    ESP_LOGI(TAG, "HV9910 initialized in safe mode: SHUTDOWN asserted (%d), DIM=0%%, LED off",
             shutdown_assert_level());

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

void hv9910_enable_at(uint8_t percent, uint32_t ramp_ms)
{
    if (percent > 100) {
        percent = 100;
    }
    hv9910_cmd_t cmd = { .type = HV_CMD_ENABLE, .ramp_ms = clamp_ramp_ms(ramp_ms), .percent = percent };
    post_cmd(&cmd, false);
}

void hv9910_enable(uint32_t ramp_ms)
{
    uint8_t target = (s_last_nonzero_percent > 0) ? s_last_nonzero_percent : DEFAULT_ON_PERCENT;
    hv9910_enable_at(target, ramp_ms);
}

void hv9910_disable(uint32_t ramp_ms)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_DISABLE, .ramp_ms = clamp_ramp_ms(ramp_ms) };
    post_cmd(&cmd, false);
}

void hv9910_emergency_disable(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_EMERGENCY_OFF };
    post_cmd(&cmd, true);
}

void hv9910_set_dim(uint8_t percent, uint32_t ramp_ms)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_SET_DIM, .ramp_ms = clamp_ramp_ms(ramp_ms), .percent = percent };
    post_cmd(&cmd, false);
}

void hv9910_identify(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_IDENTIFY };
    post_cmd(&cmd, false);
}

void hv9910_set_pending(bool on)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_SET_PENDING, .on = on };
    post_cmd(&cmd, false);
}

bool hv9910_is_ramp_pending(void)
{
    return s_pending != PENDING_NONE || s_awaiting_shutdown_fade;
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
