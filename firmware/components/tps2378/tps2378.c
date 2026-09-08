/** @file tps2378.c
 * @brief PoE/AUX power monitoring and VBUS validation implementation.
 *
 * Pin semantics (TPS2378 + this board):
 *   - CDB: hotswap power-good. Active LOW during inrush, HIGH once the bulk
 *     cap is charged / the hotswap FET has settled. It sits HIGH in nearly
 *     every steady state and says nothing about the PoE class -- it is
 *     telemetry/logging only, never a readiness or class signal.
 *   - T2P: the class selector. Active LOW; a latch asserted on a Type-2
 *     classification (2-event) OR when APD is high (AUX above the divider
 *     threshold). Set => Type-2/AUX (25.5 W); clear => Type-1 (12.95 W).
 *   - VBUS: measured bus voltage, the single source of truth for "powered".
 *
 * Ready = VBUS at operating level (debounced, with hysteresis). VBUS is the
 * only signal common to Type-1 / Type-2 / AUX, and it is also the backstop:
 * a source that cannot sustain the load (including a Type-2 PSE with T2P
 * stuck asserted) drops VBUS, so ready drops and the driver goes OFF.
 */
#include "tps2378.h"
#include "voltage_sense.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "TPS2378";

#define DEBOUNCE_SAMPLES        5
#define SAMPLE_PERIOD_MS        20
#define WAIT_LOG_PERIOD_MS      5000

#define POE_TYPE1_POWER_W       12.95f  /* 802.3af */
#define POE_TYPE2_POWER_W       25.5f   /* 802.3at Type 2 / PoE+ */

static tps2378_config_t s_config;

static EventGroupHandle_t s_evt;
#define POE_READY_BIT   BIT0

static volatile bool s_is_ready = false;
static volatile tps2378_source_t s_source = TPS2378_SOURCE_NONE;
static volatile bool s_cdb_confirmed = false;
static volatile bool s_t2p_confirmed = false;
static volatile bool s_vbus_confirmed = false;
static volatile int s_vbus_mv = 0;

/** @brief Debounce state for one boolean signal. */
typedef struct {
    bool last_sample;   /**< Last raw sample seen. */
    bool confirmed;     /**< Debounced/confirmed value. */
    int stable_count;   /**< Consecutive stable samples so far. */
} debounce_t;

/**
 * @brief Updates a debounce state with a new sample.
 * @param d Debounce state to update.
 * @param sample New raw sample.
 * @return Debounced/confirmed value after the update.
 */
static bool debounce_update(debounce_t *d, bool sample)
{
    if (sample == d->last_sample) {
        if (d->stable_count < DEBOUNCE_SAMPLES) {
            d->stable_count++;
        }
    } else {
        d->stable_count = 0;
        d->last_sample = sample;
    }
    if (d->stable_count >= DEBOUNCE_SAMPLES) {
        d->confirmed = sample;
    }
    return d->confirmed;
}

/**
 * @brief Reads the raw CDB signal (hotswap power-good).
 * @return true once the inrush is complete / the bulk cap is charged
 * (CDB HIGH). Diagnostic/logging only -- not a readiness or class signal.
 */
static bool read_cdb_inrush_done(void)
{
    return gpio_get_level(s_config.cdb_pin) != 0;
}

/**
 * @brief Reads the raw T2P signal (the class selector).
 * @return true (T2P active, LOW) when a Type-2 classification is latched or
 * APD is high (AUX); false means the Type-1 budget applies.
 */
static bool read_t2p_type2_or_aux(void)
{
    return gpio_get_level(s_config.t2p_pin) == 0;
}

/**
 * @brief Median of three integers.
 * @param a First value.
 * @param b Second value.
 * @param c Third value.
 * @return Median value.
 */
static int median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

/**
 * @brief Reads VBUS as the median of three quick ADC samples.
 * @return Voltage in millivolts (0 on read failure).
 */
static int read_vbus_mv(void)
{
    int samples[3];
    for (int i = 0; i < 3; i++) {
        voltage_reading_t v = {0};
        samples[i] = (voltage_sense_read(&v) == ESP_OK) ? v.vbus_mv : 0;
    }
    return median3(samples[0], samples[1], samples[2]);
}

/**
 * @brief Applies hysteresis to a VBUS sample.
 * @param vbus_mv Current VBUS reading, in millivolts.
 * @param currently_ok Whether VBUS is currently considered OK.
 * @return true if VBUS should be considered OK after this sample.
 */
static bool vbus_threshold_sample(int vbus_mv, bool currently_ok)
{
    int threshold = currently_ok ? (s_config.vbus_min_mv - s_config.vbus_hysteresis_mv) : s_config.vbus_min_mv;
    return vbus_mv >= threshold;
}

/**
 * @brief Background task: samples CDB/T2P/VBUS, gates "ready" on VBUS, and
 * derives the power class from T2P.
 * @param arg Unused.
 * @return Never returns.
 */
static void poe_monitor_task(void *arg)
{
    (void)arg;

    debounce_t cdb_deb = { .last_sample = read_cdb_inrush_done(), .confirmed = false, .stable_count = 0 };
    debounce_t t2p_deb = { .last_sample = read_t2p_type2_or_aux(), .confirmed = false, .stable_count = 0 };
    debounce_t vbus_deb = { .last_sample = (read_vbus_mv() >= s_config.vbus_min_mv), .confirmed = false, .stable_count = 0 };
    bool prev_inrush_done = cdb_deb.confirmed;
    bool prev_t2p_class = t2p_deb.confirmed;
    bool prev_vbus_ok = vbus_deb.confirmed;
    tps2378_source_t prev_source = TPS2378_SOURCE_NONE;

    TickType_t last_wait_log = xTaskGetTickCount();

    while (1) {
        bool cdb_inrush_done = debounce_update(&cdb_deb, read_cdb_inrush_done());
        bool t2p_class = debounce_update(&t2p_deb, read_t2p_type2_or_aux());

        int vbus_mv = read_vbus_mv();
        s_vbus_mv = vbus_mv;
        bool vbus_ok = debounce_update(&vbus_deb, vbus_threshold_sample(vbus_mv, vbus_deb.confirmed));

        s_cdb_confirmed = cdb_inrush_done;
        s_t2p_confirmed = t2p_class;
        s_vbus_confirmed = vbus_ok;

        /* --- class + ready ------------------------------------------------
         * ready is VBUS alone. The class comes from T2P and is only
         * meaningful while ready; until the T2P debounce confirms, it reads
         * clear, so an unclassified source resolves to the smaller
         * (Type-1) budget. No APD GPIO on this board, so the T2P-set branch
         * cannot tell real Type-2 from AUX -- both carry 25.5 W, reported as
         * TYPE2. */
        tps2378_source_t source = TPS2378_SOURCE_NONE;
        if (vbus_ok) {
            source = t2p_class ? TPS2378_SOURCE_TYPE2 : TPS2378_SOURCE_TYPE1;
        }
        s_source = source;

        bool new_ready = vbus_ok;
        if (new_ready != s_is_ready) {
            s_is_ready = new_ready;
            if (new_ready) {
                xEventGroupSetBits(s_evt, POE_READY_BIT);
                ESP_LOGI(TAG, "READY: powered (VBUS=%dmV), class=%s, %.2fW available",
                         vbus_mv, tps2378_source_name(s_source), tps2378_get_available_power_w());
                if (s_config.on_power_ready) {
                    s_config.on_power_ready(s_source, s_config.callback_ctx);
                }
            } else {
                xEventGroupClearBits(s_evt, POE_READY_BIT);
                ESP_LOGW(TAG, "POWER LOST: VBUS=%dmV < %dmV — source not sustaining the load "
                              "(or unpowered); driver forced OFF",
                         vbus_mv, s_config.vbus_min_mv - s_config.vbus_hysteresis_mv);
                if (s_config.on_power_lost) {
                    s_config.on_power_lost(s_config.callback_ctx);
                }
            }
        }

        if (source != prev_source) {
            prev_source = source;
            ESP_LOGI(TAG, "EVENT: source class -> %s", tps2378_source_name(source));
            if (s_config.on_source_changed) {
                s_config.on_source_changed(source, s_config.callback_ctx);
            }
        }

        /* --- edge logs (CDB and T2P are telemetry, not gates) ----------- */
        if (cdb_inrush_done != prev_inrush_done) {
            prev_inrush_done = cdb_inrush_done;
            if (cdb_inrush_done) {
                ESP_LOGI(TAG, "EVENT: CDB high — hotswap inrush complete, bulk cap charged");
            } else {
                ESP_LOGW(TAG, "EVENT: CDB low — inrush in progress / hotswap FET not settled");
            }
        }
        if (t2p_class != prev_t2p_class) {
            prev_t2p_class = t2p_class;
            if (t2p_class) {
                ESP_LOGI(TAG, "EVENT: T2P active — Type-2 classification or AUX (25.5 W budget)");
            } else {
                ESP_LOGI(TAG, "EVENT: T2P inactive — Type-1 budget (12.95 W)");
            }
        }
        if (vbus_ok != prev_vbus_ok) {
            prev_vbus_ok = vbus_ok;
            if (vbus_ok) {
                ESP_LOGI(TAG, "EVENT: VBUS OK — %dmV >= %dmV threshold", vbus_mv, s_config.vbus_min_mv);
            } else {
                ESP_LOGW(TAG, "EVENT: VBUS too low — %dmV < %dmV threshold", vbus_mv,
                         s_config.vbus_min_mv - s_config.vbus_hysteresis_mv);
            }
        }

        if (!s_is_ready) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_wait_log) >= pdMS_TO_TICKS(WAIT_LOG_PERIOD_MS)) {
                last_wait_log = now;
                ESP_LOGI(TAG, "Waiting for power — VBUS=%dmV (need >=%dmV) [CDB=%d T2P=%d]",
                         vbus_mv, s_config.vbus_min_mv, cdb_inrush_done, t2p_class);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void tps2378_init(const tps2378_config_t *config)
{
    s_config = *config;

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << s_config.cdb_pin) | (1ULL << s_config.t2p_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate failed (out of memory?) -- cannot safely monitor PoE/AUX, rebooting");
        esp_restart();
    }

    BaseType_t task_ok = xTaskCreate(poe_monitor_task, "tps2378_monitor", 3072, NULL, tskIDLE_PRIORITY + 4, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(tps2378_monitor) failed -- cannot safely monitor PoE/AUX, rebooting");
        esp_restart();
    }
}

bool tps2378_wait_ready(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_evt, POE_READY_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & POE_READY_BIT) != 0;
}

bool tps2378_is_ready(void)
{
    return s_is_ready;
}

tps2378_source_t tps2378_get_source(void)
{
    return s_source;
}

bool tps2378_cdb_confirmed(void)
{
    return s_cdb_confirmed;
}

bool tps2378_t2p_confirmed(void)
{
    return s_t2p_confirmed;
}

bool tps2378_vbus_confirmed(void)
{
    return s_vbus_confirmed;
}

bool tps2378_cdb_raw(void)
{
    return read_cdb_inrush_done();
}

bool tps2378_t2p_raw(void)
{
    return read_t2p_type2_or_aux();
}

bool tps2378_vbus_raw(void)
{
    return read_vbus_mv() >= s_config.vbus_min_mv;
}

int tps2378_get_vbus_mv(void)
{
    return s_vbus_mv;
}

const char *tps2378_source_name(tps2378_source_t source)
{
    switch (source) {
    case TPS2378_SOURCE_TYPE1: return "PoE Type-1/802.3af (12.95 W)";
    case TPS2378_SOURCE_TYPE2: return "PoE Type-2/802.3at or AUX (25.5 W)";
    case TPS2378_SOURCE_AUX:   return "AUX supply (25.5 W)";
    default:                    return "none";
    }
}

const char *tps2378_source_short_name(tps2378_source_t source)
{
    switch (source) {
    case TPS2378_SOURCE_TYPE1: return "type1";
    case TPS2378_SOURCE_TYPE2: return "type2";
    case TPS2378_SOURCE_AUX:   return "aux";
    default:                    return "none";
    }
}

float tps2378_get_available_power_w(void)
{
    /* Invariant: > 0 only when ready -- s_source is NONE whenever VBUS is
     * below the operating threshold. AUX shares the Type-2 budget. */
    switch (s_source) {
    case TPS2378_SOURCE_TYPE1: return POE_TYPE1_POWER_W;
    case TPS2378_SOURCE_TYPE2:
    case TPS2378_SOURCE_AUX:   return POE_TYPE2_POWER_W;
    default:                    return 0.0f;
    }
}
