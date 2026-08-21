/*
 * See hv9910.h for the public API and the polarity/persistence notes.
 *
 * CONCURRENCY DESIGN: every physical GPIO/LEDC operation, and every piece
 * of mutable state (s_enabled, s_dim_percent, ...), belongs EXCLUSIVELY to
 * hv9910_task below. Nothing else in this file, and nothing outside it,
 * ever calls gpio_set_level()/ledc_*() directly. The public API functions
 * (hv9910_enable/disable/set_dim/identify/emergency_disable) are thin
 * wrappers that build a hv9910_cmd_t and post it to s_cmd_queue -- that's
 * the ONLY way any of this module's state or hardware ever changes.
 *
 * This exists because, before this design, tps2378's monitor task (via
 * its power-ready/power-lost callbacks, now wired from main.c -- see
 * tps2378.h), admin_channel's task, admin_channel's identify_blink_task,
 * and this module's own short-lived "assert SHUTDOWN after the ramp
 * finishes" task could all end up calling into hv9910's old direct-GPIO
 * API from different task contexts at overlapping times -- e.g. a ramped OFF
 * spawning a task that, moments later, would still assert SHUTDOWN even
 * though the driver had since been turned back ON by an unrelated command
 * (the "OFF-then-ON" bug), or an in-progress IDENTIFY blink racing a
 * concurrent DIM/OFF and clobbering whichever one finished last. A single
 * serialized task with a command queue removes the race by construction:
 * there is only ever one thing touching the hardware at a time, in the
 * exact order commands were issued (FIFO), and any newly-arriving command
 * unconditionally cancels whatever this task was in the middle of
 * waiting out (a deferred SHUTDOWN, an in-progress IDENTIFY step) before
 * acting on it -- see hv9910_task()'s main loop.
 *
 * "Immediately" for the PoE-loss safety cutoff: hv9910_emergency_disable()
 * (main.c's on_poe_power_lost(), the ONLY way tps2378 losing power turns
 * this driver off -- wired as a callback from tps2378.c, which has no
 * direct link to this module at all, see tps2378.h) posts to the
 * FRONT of the queue instead of the back, so it preempts any backlog of
 * ordinary commands. And because FreeRTOS queues wake a task blocked in
 * xQueueReceive() the instant something is posted -- regardless of how
 * long a timeout it was given -- this preemption is immediate even while
 * hv9910_task is waiting out a multi-second IDENTIFY step or a deferred
 * post-ramp SHUTDOWN; it is never delayed by that wait. The only latency
 * is the cost of already-in-flight command processing, which is always a
 * handful of non-blocking register writes (microseconds, not
 * milliseconds -- ledc_fade_start() uses LEDC_FADE_NO_WAIT).
 */
#include "hv9910.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "HV9910";

#define LEDC_SPEED_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_CHANNEL      LEDC_CHANNEL_0
#define LEDC_DUTY_RES     LEDC_TIMER_10_BIT
#define LEDC_DUTY_MAX     ((1 << 10) - 1)

#define NVS_NAMESPACE     "hv9910"
#define NVS_KEY_DIM       "dim"
#define NVS_KEY_ON        "on"
#define DEFAULT_RESTORE_PERCENT   100  /* used the very first time, if nothing was ever saved */

#define TASK_STACK_SIZE    3072
#define TASK_PRIORITY       (tskIDLE_PRIORITY + 6) /* highest app task -- final actuator for the safety cutoff */
#define CMD_QUEUE_LEN        8
#define CMD_QUEUE_SEND_TIMEOUT_MS   100  /* ordinary (non-emergency) senders only -- see post_cmd() */

#define IDENTIFY_BLINK_CYCLES     6
#define IDENTIFY_BLINK_PERIOD_MS  400

/* ---------------------------------------------------------------------
 * Board wiring/polarity, copied in from hv9910_init()'s config argument --
 * everything below reads this instead of a board-specific compile-time
 * macro, see the file header.
 * --------------------------------------------------------------------- */
static hv9910_config_t s_config;

static int shutdown_assert_level(void) { return s_config.shutdown_active_high ? 1 : 0; }
static int shutdown_run_level(void)    { return s_config.shutdown_active_high ? 0 : 1; }

/* ---------------------------------------------------------------------
 * State -- owned exclusively by hv9910_task. Marked volatile because the
 * getters (hv9910_is_enabled() etc.) are read from other tasks.
 * --------------------------------------------------------------------- */
static volatile bool s_enabled = false;
static volatile uint8_t s_dim_percent = 0;          /* current live brightness, can be 0 while off */
static volatile uint8_t s_last_nonzero_percent = 0; /* remembered resume level, persisted to NVS */
static volatile bool s_last_enabled_persisted = false; /* remembered on/off state, persisted to NVS -- see hv9910_was_last_on() */
static bool s_ledc_started = false;
static nvs_handle_t s_nvs_handle;
static bool s_nvs_ok = false;

/* ---------------------------------------------------------------------
 * Command queue
 * --------------------------------------------------------------------- */
typedef enum {
    HV_CMD_ENABLE,
    HV_CMD_DISABLE,
    HV_CMD_SET_DIM,
    HV_CMD_IDENTIFY,
} hv9910_cmd_type_t;

typedef struct {
    hv9910_cmd_type_t type;
    uint32_t ramp_ms;   /* ENABLE, DISABLE, SET_DIM */
    bool persist;        /* ENABLE, DISABLE */
    uint8_t percent;     /* SET_DIM */
} hv9910_cmd_t;

static QueueHandle_t s_cmd_queue;
static bool s_task_ready = false; /* true once init succeeded -- guards the public API against a NULL queue */

/* Deferred/multi-step actions the task is waiting to fire, unless a new
 * command arrives first and cancels it -- see hv9910_task(). */
typedef enum {
    PENDING_NONE,
    PENDING_SHUTDOWN,       /* assert SHUTDOWN once a ramped-OFF's fade-to-0 finishes */
    PENDING_IDENTIFY_STEP,  /* advance to the next step of an in-progress IDENTIFY blink */
} pending_action_t;

static pending_action_t s_pending = PENDING_NONE;
static TickType_t s_pending_deadline;

static int s_identify_steps_left;
static bool s_identify_next_is_on;
static bool s_identify_saved_enabled;
static uint8_t s_identify_saved_dim;

static void shutdown_write(int level)
{
    gpio_set_level(s_config.shutdown_pin, level);
}

static uint32_t clamp_ramp_ms(uint32_t ramp_ms)
{
    if (ramp_ms > s_config.max_ramp_ms) {
        ESP_LOGW(TAG, "ramp_ms=%" PRIu32 " exceeds the configured max (%" PRIu32 ") -- clamping",
                 ramp_ms, s_config.max_ramp_ms);
        return s_config.max_ramp_ms;
    }
    return ramp_ms;
}

static void nvs_init_and_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition layout changed or is corrupt: erase and retry once. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s) — brightness won't persist across reboots", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s) — brightness won't persist across reboots", esp_err_to_name(err));
        return;
    }
    s_nvs_ok = true;

    uint8_t saved = 0;
    err = nvs_get_u8(s_nvs_handle, NVS_KEY_DIM, &saved);
    if (err == ESP_OK && saved > 0 && saved <= 100) {
        s_last_nonzero_percent = saved;
        ESP_LOGI(TAG, "Restored last brightness from NVS: %u%%", saved);
    } else {
        s_last_nonzero_percent = DEFAULT_RESTORE_PERCENT;
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read of '%s' failed (%s) — defaulting to %u%%",
                     NVS_KEY_DIM, esp_err_to_name(err), DEFAULT_RESTORE_PERCENT);
        }
    }

    uint8_t saved_on = 0;
    err = nvs_get_u8(s_nvs_handle, NVS_KEY_ON, &saved_on);
    if (err == ESP_OK) {
        s_last_enabled_persisted = (saved_on != 0);
        ESP_LOGI(TAG, "Restored last on/off state from NVS: %s", s_last_enabled_persisted ? "ON" : "off");
    } else {
        s_last_enabled_persisted = false; /* a fresh device never auto-resumes on its own */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read of '%s' failed (%s) — defaulting to off", NVS_KEY_ON, esp_err_to_name(err));
        }
    }
}

/* Persists 'percent' as the new remembered resume level, but only if it
 * actually differs from what's already stored — avoids a flash write on
 * every single hv9910_enable() re-applying the same value. */
static void persist_last_nonzero(uint8_t percent)
{
    if (percent == 0 || percent == s_last_nonzero_percent) {
        return;
    }
    s_last_nonzero_percent = percent;
    if (!s_nvs_ok) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs_handle, NVS_KEY_DIM, percent);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist brightness to NVS (%s)", esp_err_to_name(err));
    }
}

/* Persists 'enabled' as the new remembered on/off state -- see
 * hv9910_enable()/hv9910_disable()/hv9910_was_last_on(). Only writes to
 * NVS if it actually changed. */
static void persist_enabled_state(bool enabled)
{
    if (enabled == s_last_enabled_persisted) {
        return;
    }
    s_last_enabled_persisted = enabled;
    if (!s_nvs_ok) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs_handle, NVS_KEY_ON, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist on/off state to NVS (%s)", esp_err_to_name(err));
    }
}

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

    /* Installs the LEDC fade service (interrupt-driven hardware duty
     * ramp) -- required once before any ledc_set_fade_with_time()/
     * ledc_fade_start() call. Only ever reached once, guarded by
     * s_ledc_started above. */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    s_ledc_started = true;
}

/* --- Internal (task-only) implementations. Never call these outside
 * hv9910_task -- see the file header. Public API functions below only
 * ever post commands; dispatch_cmd()/fire_pending_action() are the only
 * callers of these. --- */

static void hv_do_set_dim(uint8_t percent, uint32_t ramp_ms)
{
    if (percent > 100) {
        percent = 100;
    }
    s_dim_percent = percent;

    ledc_start_if_needed();

    /* NOTE: classic ESP32's LEDC has no ledc_fade_stop()
     * (SOC_LEDC_SUPPORT_FADE_STOP == 0) -- a fade already in flight
     * cannot be cancelled or retargeted at all. Calling
     * ledc_set_fade_with_time()/ledc_fade_start() again while a previous
     * LEDC_FADE_NO_WAIT fade on this same channel hasn't finished yet
     * BLOCKS this call until that fade completes (it holds a semaphore
     * the whole time), and by then the duty has usually already reached
     * that first fade's target -- so the second fade has nothing left to
     * do and looks like it never happened. This can't happen here: every
     * fade-starting call goes through this same serialized task, one
     * command at a time, and hv9910_enable() only ever pairs a ramp_ms=0
     * (non-fade) call with a real ramped one -- see hv9910.h. */
    uint32_t duty = ((uint32_t)percent * LEDC_DUTY_MAX) / 100;

    if (ramp_ms == 0) {
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
    } else {
        ledc_set_fade_with_time(LEDC_SPEED_MODE, LEDC_CHANNEL, duty, (int)ramp_ms);
        /* NO_WAIT: returns immediately, the fade runs in the background
         * via the LEDC peripheral's own hardware/interrupt-driven duty
         * ramp -- never blocks hv9910_task for the ramp duration. */
        ledc_fade_start(LEDC_SPEED_MODE, LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
    }

    persist_last_nonzero(percent);

    ESP_LOGI(TAG, "Dimming set to %u%% over %ums (duty=%u/%u)", percent, (unsigned)ramp_ms, (unsigned)duty, LEDC_DUTY_MAX);
}

static void hv_do_enable(uint32_t ramp_ms, bool persist)
{
    if (persist) {
        persist_enabled_state(true);
    }

    uint8_t restore_percent = (s_last_nonzero_percent > 0) ? s_last_nonzero_percent : DEFAULT_RESTORE_PERCENT;

    /* SHUTDOWN released FIRST, then the fade starts -- ramping the duty
     * before the driver chip is actually running would just mean the fade
     * plays out (in the background) while SHUTDOWN is still asserted and
     * nothing visible happens yet. */
    shutdown_write(shutdown_run_level());
    s_enabled = true;
    hv_do_set_dim(restore_percent, ramp_ms);
    ESP_LOGW(TAG, "Driver ENABLED (SHUTDOWN released), ramping to %u%% over %ums%s",
             restore_percent, (unsigned)ramp_ms, persist ? "" : " (transient, not persisted)");
}

static void hv_do_disable(uint32_t ramp_ms, bool persist)
{
    if (persist) {
        persist_enabled_state(false);
    }

    /* Ramp the brightness down first (or snap to 0 instantly if
     * ramp_ms==0) -- SHUTDOWN is only asserted once the driver is
     * actually visually off, so a ramped OFF looks like a fade, not an
     * abrupt cut. */
    hv_do_set_dim(0, ramp_ms);

    if (ramp_ms == 0) {
        shutdown_write(shutdown_assert_level());
        s_enabled = false;
        ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted)");
    } else {
        /* Defer the actual SHUTDOWN assert until the fade-to-0 finishes,
         * WITHOUT spawning a separate task for it (that's the old design
         * -- see the file header for why it was unsafe). hv9910_task's
         * own main loop fires this when its queue-receive times out at
         * s_pending_deadline, unless a new command cancels it first. */
        s_pending = PENDING_SHUTDOWN;
        s_pending_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ramp_ms);
    }
}

/* Displays the next step of an in-progress IDENTIFY sequence and
 * schedules the one after it, or -- once all steps are done -- restores
 * the pre-identify state and ends the sequence. Called once synchronously
 * to kick off the first step, and then once per step by
 * fire_pending_action(). */
static void do_identify_step(void)
{
    if (s_identify_steps_left <= 0) {
        if (s_identify_saved_enabled) {
            hv_do_set_dim(s_identify_saved_dim, 0);
        } else {
            hv_do_disable(0, false);
        }
        s_pending = PENDING_NONE;
        return;
    }

    hv_do_set_dim(s_identify_next_is_on ? 100 : 0, 0);
    s_identify_next_is_on = !s_identify_next_is_on;
    s_identify_steps_left--;

    s_pending = PENDING_IDENTIFY_STEP;
    s_pending_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IDENTIFY_BLINK_PERIOD_MS);
}

static void hv_do_identify(void)
{
    s_identify_saved_enabled = s_enabled;
    s_identify_saved_dim = s_dim_percent;

    if (!s_enabled) {
        /* hv_do_set_dim() alone only changes the PWM duty (DIM pin) -- it
         * never touches SHUTDOWN. Without releasing it here, blinking
         * while the driver was off (the common case, e.g. right after
         * boot with nothing turned on yet) would silently produce no
         * visible light at all. ramp_ms=0 and persist=false -- this is a
         * transient blink, not the operator asking for the driver to stay
         * on, so it must NOT change the remembered on/off state, and
         * shouldn't fade in before the blink even starts. */
        hv_do_enable(0, false);
    }

    s_identify_steps_left = IDENTIFY_BLINK_CYCLES * 2; /* each cycle = one OFF step + one ON step */
    s_identify_next_is_on = false; /* first step shows OFF, matching the original blink order */
    do_identify_step();
}

static void dispatch_cmd(const hv9910_cmd_t *cmd)
{
    switch (cmd->type) {
    case HV_CMD_ENABLE:
        hv_do_enable(cmd->ramp_ms, cmd->persist);
        break;
    case HV_CMD_DISABLE:
        hv_do_disable(cmd->ramp_ms, cmd->persist);
        break;
    case HV_CMD_SET_DIM:
        hv_do_set_dim(cmd->percent, cmd->ramp_ms);
        break;
    case HV_CMD_IDENTIFY:
        hv_do_identify();
        break;
    }
}

/* Called when hv9910_task's queue-receive times out at s_pending_deadline
 * with nothing new having arrived -- i.e. whatever was pending (a
 * deferred SHUTDOWN, or the next IDENTIFY step) is due now. */
static void fire_pending_action(void)
{
    switch (s_pending) {
    case PENDING_SHUTDOWN:
        shutdown_write(shutdown_assert_level());
        s_enabled = false;
        /* Second layer of safety, same as the instant path in
         * hv_do_disable(): make sure the duty is really at 0, not just
         * "the fade that was ramping to 0 probably finished by now". */
        if (s_ledc_started) {
            ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
            ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
        }
        ESP_LOGI(TAG, "Driver DISABLED (SHUTDOWN asserted, after ramp)");
        s_pending = PENDING_NONE;
        break;
    case PENDING_IDENTIFY_STEP:
        do_identify_step(); /* leaves s_pending set again, or clears it once done */
        break;
    case PENDING_NONE:
    default:
        break;
    }
}

static void hv9910_task(void *arg)
{
    (void)arg;

    while (1) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (s_pending != PENDING_NONE) {
            /* Subtraction, not a direct comparison of absolute tick
             * counts -- wraps correctly across the tick counter's own
             * rollover (~49.7 days at the default 1kHz tick), same idiom
             * used elsewhere in this codebase (tps2378.c,
             * admin_channel.c's nonce TTL). A direct ">" comparison of
             * s_pending_deadline vs "now" would misfire right at that
             * rollover boundary. */
            TickType_t remaining = s_pending_deadline - xTaskGetTickCount();
            wait_ticks = ((int32_t)remaining > 0) ? remaining : 0;
        }

        hv9910_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, wait_ticks) == pdTRUE) {
            /* Any real command cancels whatever was pending -- this is
             * exactly what fixes the "OFF-then-ON leaves a stale deferred
             * SHUTDOWN" bug, and what lets a real command interrupt an
             * in-progress IDENTIFY blink instead of racing it. The
             * command itself, dispatched right below, is the new state;
             * there's no need to separately "restore" anything. */
            s_pending = PENDING_NONE;
            dispatch_cmd(&cmd);
        } else if (s_pending != PENDING_NONE) {
            fire_pending_action();
        }
    }
}

/* Posts a command from any task. 'to_front' is only ever true for
 * hv9910_emergency_disable() -- see hv9910.h. Ordinary sends use a
 * bounded timeout (never portMAX_DELAY): this queue is only ever this
 * deep if something is very wrong, and no caller -- especially not the
 * safety-critical paths -- should be able to block forever on it. */
static void post_cmd(const hv9910_cmd_t *cmd, bool to_front)
{
    if (!s_task_ready) {
        ESP_LOGE(TAG, "hv9910 command queue not ready (init failed?) -- command type %d dropped", (int)cmd->type);
        return;
    }

    if (to_front) {
        /* The emergency cutoff must never be dropped, even in the
         * pathological case where the queue happens to be completely
         * full at this exact instant -- a non-blocking send-to-front
         * alone could still fail then. If it does, forcibly discard the
         * single OLDEST queued command to make room and retry: losing
         * one stale ordinary command is an acceptable price for
         * guaranteeing the power-loss cutoff always gets through. */
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
    /* Copied, not just pointer-retained -- config doesn't need to stay
     * valid after this call returns (see hv9910.h). */
    s_config = *config;

    /* --- 1) SHUTDOWN: the very first GPIO configured in the whole
     * firmware. Runs synchronously, before s_cmd_queue/hv9910_task even
     * exist -- ESP_ERROR_CHECK on purpose: this is the one GPIO
     * configuration failure in the whole firmware where "keep running
     * anyway" is worse than a clean reboot, since we can no longer
     * guarantee the driver is actually off. --- */
    gpio_config_t shutdown_cfg = {
        .pin_bit_mask = 1ULL << s_config.shutdown_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&shutdown_cfg));
    shutdown_write(shutdown_assert_level()); /* guarantee OFF before anything else */

    /* --- 2) DIMMING: while LEDC isn't configured yet, keep the pin as a
     * plain GPIO, forced to the electrical level that corresponds to "0%
     * brightness" on the HV9910 side (remember the logic may be externally
     * inverted: dim_active_high=true means 0% logical = ESP32 pin HIGH).
     * This avoids any floating state in the window between boot and LEDC
     * configuration. */
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
    s_ledc_started = false;

    /* --- 3) Load the remembered brightness/on-off state from NVS (doesn't
     * touch hardware — the driver stays safely off until hv9910_enable()). --- */
    nvs_init_and_load();

    ESP_LOGI(TAG, "HV9910 initialized in safe mode: SHUTDOWN=%d (assert=%d), DIM=0%% (resume level: %u%%)",
             shutdown_assert_level(), shutdown_assert_level(), s_last_nonzero_percent);

    /* --- 4) Command queue + the single task that owns all hardware
     * access from here on. If either fails to come up, the driver simply
     * stays in the safe, disabled state configured above forever -- the
     * public API functions all check s_task_ready and refuse to do
     * anything (logging loudly) rather than touch a queue/task that
     * doesn't exist. --- */
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

void hv9910_enable(uint32_t ramp_ms, bool persist)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_ENABLE, .ramp_ms = clamp_ramp_ms(ramp_ms), .persist = persist };
    post_cmd(&cmd, false);
}

void hv9910_disable(uint32_t ramp_ms, bool persist)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_DISABLE, .ramp_ms = clamp_ramp_ms(ramp_ms), .persist = persist };
    post_cmd(&cmd, false);
}

void hv9910_emergency_disable(void)
{
    hv9910_cmd_t cmd = { .type = HV_CMD_DISABLE, .ramp_ms = 0, .persist = false };
    post_cmd(&cmd, true); /* jump the queue -- see hv9910.h */
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

uint8_t hv9910_get_dim_percent(void)
{
    return s_dim_percent;
}

uint8_t hv9910_get_last_nonzero_percent(void)
{
    return s_last_nonzero_percent;
}

bool hv9910_was_last_on(void)
{
    return s_last_enabled_persisted;
}

bool hv9910_is_enabled(void)
{
    return s_enabled;
}
