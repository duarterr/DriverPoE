/** @file tps2378.c
 * @brief PoE/AUX power monitoring and VBUS validation implementation.
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
 * @brief Reads the raw CDB signal.
 * @return true if a real PoE source is negotiated and stable.
 */
static bool read_cdb_poe_ok(void)
{
    return gpio_get_level(s_config.cdb_pin) != 0;
}

/**
 * @brief Reads the raw T2P signal.
 * @return true if Type-2 classification or AUX presence is indicated.
 */
static bool read_t2p_aux_or_type2(void)
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
 * @brief Background task that samples CDB/T2P/VBUS and updates the ready state.
 * @param arg Unused.
 * @return Never returns.
 */
static void poe_monitor_task(void *arg)
{
    (void)arg;

    debounce_t cdb_deb = { .last_sample = read_cdb_poe_ok(), .confirmed = false, .stable_count = 0 };
    debounce_t t2p_deb = { .last_sample = read_t2p_aux_or_type2(), .confirmed = false, .stable_count = 0 };
    debounce_t vbus_deb = { .last_sample = (read_vbus_mv() >= s_config.vbus_min_mv), .confirmed = false, .stable_count = 0 };
    bool prev_poe_ok = cdb_deb.confirmed;
    bool prev_aux_or_type2 = t2p_deb.confirmed;
    bool prev_vbus_ok = vbus_deb.confirmed;

    TickType_t last_wait_log = xTaskGetTickCount();

    while (1) {
        bool poe_ok = debounce_update(&cdb_deb, read_cdb_poe_ok());
        bool aux_or_type2 = debounce_update(&t2p_deb, read_t2p_aux_or_type2());

        int vbus_mv = read_vbus_mv();
        s_vbus_mv = vbus_mv;
        bool vbus_ok = debounce_update(&vbus_deb, vbus_threshold_sample(vbus_mv, vbus_deb.confirmed));

        s_cdb_confirmed = poe_ok;
        s_t2p_confirmed = aux_or_type2;
        s_vbus_confirmed = vbus_ok;

        if (poe_ok != prev_poe_ok) {
            prev_poe_ok = poe_ok;
            if (poe_ok) {
                ESP_LOGI(TAG, "EVENT: CDB asserted — real PoE negotiated (inrush done)");
            } else {
                ESP_LOGW(TAG, "EVENT: CDB dropped — no real PoE negotiated (unpowered/inrush/lost)");
            }
        }
        if (aux_or_type2 != prev_aux_or_type2) {
            prev_aux_or_type2 = aux_or_type2;
            if (aux_or_type2) {
                ESP_LOGI(TAG, "EVENT: T2P asserted — AUX supply or Type-2 PoE present");
            } else {
                ESP_LOGW(TAG, "EVENT: T2P dropped — no AUX/Type-2 confirmation anymore");
            }
        }
        if (vbus_ok != prev_vbus_ok) {
            prev_vbus_ok = vbus_ok;
            if (vbus_ok) {
                ESP_LOGI(TAG, "EVENT: VBUS OK — %dmV >= %dmV threshold", vbus_mv, s_config.vbus_min_mv);
            } else {
                ESP_LOGW(TAG, "EVENT: VBUS too low — %dmV < %dmV threshold", vbus_mv, s_config.vbus_min_mv);
            }
        }

        tps2378_source_t digital_source = TPS2378_SOURCE_NONE;
        if (poe_ok) {
            digital_source = aux_or_type2 ? TPS2378_SOURCE_TYPE2 : TPS2378_SOURCE_TYPE1;
        } else if (aux_or_type2) {
            digital_source = TPS2378_SOURCE_AUX;
        }
        s_source = digital_source;

        bool digital_source_ok = (digital_source != TPS2378_SOURCE_NONE);
        bool new_ready = digital_source_ok && vbus_ok;

        if (new_ready != s_is_ready) {
            s_is_ready = new_ready;
            if (new_ready) {
                xEventGroupSetBits(s_evt, POE_READY_BIT);
                ESP_LOGI(TAG, "READY: %s (CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV), %.2fW available",
                         tps2378_source_name(s_source), poe_ok, aux_or_type2, vbus_mv,
                         tps2378_get_available_power_w());
                if (s_config.on_power_ready) {
                    s_config.on_power_ready(s_source, s_config.callback_ctx);
                }
            } else {
                xEventGroupClearBits(s_evt, POE_READY_BIT);
                if (s_config.on_power_lost) {
                    s_config.on_power_lost(s_config.callback_ctx);
                }
                if (digital_source_ok && !vbus_ok) {
                    ESP_LOGW(TAG, "LOW POWER MODE: digital source OK but VBUS too low "
                                  "(CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV < %dmV) — driver forced OFF",
                             poe_ok, aux_or_type2, vbus_mv, s_config.vbus_min_mv);
                } else {
                    ESP_LOGW(TAG, "LOW POWER MODE: neither PoE nor AUX confirmed anymore "
                                  "(CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV) — driver forced OFF",
                             poe_ok, aux_or_type2, vbus_mv);
                }
            }
        }

        if (!s_is_ready) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_wait_log) >= pdMS_TO_TICKS(WAIT_LOG_PERIOD_MS)) {
                last_wait_log = now;
                ESP_LOGI(TAG, "Still in low power mode — CDB poe_ok=%d T2P aux_or_type2=%d VBUS=%dmV (need >=%dmV), "
                              "waiting for PoE or AUX...",
                         poe_ok, aux_or_type2, vbus_mv, s_config.vbus_min_mv);
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
    return read_cdb_poe_ok();
}

bool tps2378_t2p_raw(void)
{
    return read_t2p_aux_or_type2();
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
    case TPS2378_SOURCE_TYPE1: return "PoE Type-1/802.3af";
    case TPS2378_SOURCE_TYPE2: return "PoE Type-2/802.3at";
    case TPS2378_SOURCE_AUX:   return "AUX bench supply (not PoE)";
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
    switch (s_source) {
    case TPS2378_SOURCE_TYPE1: return POE_TYPE1_POWER_W;
    case TPS2378_SOURCE_TYPE2: return POE_TYPE2_POWER_W;
    default:                    return 0.0f;
    }
}
