#include "tps2378.h"
#include "voltage_sense.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "TPS2378";

#define DEBOUNCE_SAMPLES        5     /* consecutive stable readings needed to confirm a transition */
#define SAMPLE_PERIOD_MS        20
#define WAIT_LOG_PERIOD_MS      5000  /* heartbeat log while stuck in low-power mode */

/* Guaranteed PD power per IEEE 802.3af/at, in watts (standard headline
 * figures, not a live measurement):
 * https://www.fs.com/blog/understanding-poe-standards-and-wattage-21.html */
#define POE_TYPE1_POWER_W       12.95f  /* 802.3af */
#define POE_TYPE2_POWER_W       25.5f   /* 802.3at Type 2 / PoE+ */

/* ---------------------------------------------------------------------
 * Board wiring/thresholds, copied in from tps2378_init()'s config
 * argument -- see the file header.
 * --------------------------------------------------------------------- */
static tps2378_config_t s_config;

static EventGroupHandle_t s_evt;
#define POE_READY_BIT   BIT0

static volatile bool s_is_ready = false;
static volatile tps2378_source_t s_source = TPS2378_SOURCE_NONE;
static volatile bool s_cdb_confirmed = false;
static volatile bool s_t2p_confirmed = false;
static volatile bool s_vbus_confirmed = false;
static volatile int s_vbus_mv = 0;

typedef struct {
    bool last_sample;
    bool confirmed;
    int stable_count;
} debounce_t;

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

static bool read_cdb_poe_ok(void)
{
    /* CDB active LOW = still negotiating/inrush-limiting. HIGH = a real
     * PoE source (Type-1 or Type-2) is stable and released. */
    return gpio_get_level(s_config.cdb_pin) != 0;
}

static bool read_t2p_aux_or_type2(void)
{
    /* T2P active LOW = either Type-2 classification was observed, or the
     * AUX (>40V) divider is forcing the TPS2378's APD pin high. Either
     * way, it means "safe to operate" — see tps2378.h. */
    return gpio_get_level(s_config.t2p_pin) == 0;
}

/* Cheap 3-sample median filter -- rejects a single noisy/glitched ADC
 * reading without the cost or latency of a larger moving average. Three
 * comparisons, no allocation, no history buffer. */
static int median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

/* Reads VBUS and returns the mV value; also used by the "raw" public
 * getter. Takes 3 quick ADC samples and returns their median instead of
 * a single reading -- cheap noise rejection on top of the temporal
 * debounce_update() below applies afterwards. A failed individual sample
 * reads as 0 ("not ok"), same as before. */
static int read_vbus_mv(void)
{
    int samples[3];
    for (int i = 0; i < 3; i++) {
        voltage_reading_t v = {0};
        samples[i] = (voltage_sense_read(&v) == ESP_OK) ? v.vbus_mv : 0;
    }
    return median3(samples[0], samples[1], samples[2]);
}

/* Schmitt-trigger style hysteresis: while VBUS is currently NOT confirmed
 * ok, require the higher config.vbus_min_mv threshold to become ok; while
 * it IS currently confirmed ok, require dropping below the lower
 * (vbus_min_mv - vbus_hysteresis_mv) threshold to stop being ok. Without
 * this, a VBUS reading sitting right at ~40V could flip the debounced
 * verdict back and forth on ordinary ripple/noise. See tps2378_config_t
 * for the margin and debounce_update() below for the temporal debounce
 * layered on top of this. */
static bool vbus_threshold_sample(int vbus_mv, bool currently_ok)
{
    int threshold = currently_ok ? (s_config.vbus_min_mv - s_config.vbus_hysteresis_mv) : s_config.vbus_min_mv;
    return vbus_mv >= threshold;
}

static void poe_monitor_task(void *arg)
{
    (void)arg;

    debounce_t cdb_deb = { .last_sample = read_cdb_poe_ok(), .confirmed = false, .stable_count = 0 };
    debounce_t t2p_deb = { .last_sample = read_t2p_aux_or_type2(), .confirmed = false, .stable_count = 0 };
    debounce_t vbus_deb = { .last_sample = (read_vbus_mv() >= s_config.vbus_min_mv), .confirmed = false, .stable_count = 0 }; /* boots "not ok"; the hysteresis in the loop below only matters once confirmed==true */
    /* Tracks each raw signal's own last logged state, independently of the
     * combined ready/not-ready verdict below — see the EVENT logs. */
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

        /* Log each raw signal's own confirmed transition independently of
         * whether it changes the overall ready/not-ready verdict below —
         * e.g. real PoE (CDB) can drop while AUX (T2P) is still holding
         * the system ready, and that's still worth knowing about. This is
         * the "event" other logic can key off later to decide whether to
         * shut down anything non-essential when a specific source is
         * lost, even if the system as a whole is still up on the other
         * source. */
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

        /* The digital source is tracked independently of VBUS and updated
         * every cycle (not just on the combined ready transition below),
         * so the source stays informative even while low-power mode is
         * caused purely by VBUS being too low (CDB/T2P can be confirmed
         * while ready is still 0 — check vbus_ok/vbus_mv to tell the two
         * apart, see tps2378.h). */
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
                /* Notify the caller's callback, if any -- e.g. main.c's
                 * wiring resumes the HV9910 driver here if it was on
                 * before power was lost. This module has no idea what (if
                 * anything) is downstream -- see tps2378.h. */
                if (s_config.on_power_ready) {
                    s_config.on_power_ready(s_source, s_config.callback_ctx);
                }
            } else {
                xEventGroupClearBits(s_evt, POE_READY_BIT);
                /* Notify the caller's callback FIRST, before any logging
                 * below -- e.g. main.c's wiring immediately cuts the
                 * HV9910 driver here, even if it was already turned on by
                 * a remote command or is mid-IDENTIFY-blink (see
                 * hv9910.h). Calling this before the log lines keeps the
                 * latency as low as this module can make it -- see
                 * tps2378.h. */
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

            /* Periodic reminder so the console clearly shows the system
             * is alive and still evaluating, even if no new transition
             * has happened in a while. */
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
    /* Copied, not just pointer-retained -- config doesn't need to stay
     * valid after this call returns (see tps2378.h). */
    s_config = *config;

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << s_config.cdb_pin) | (1ULL << s_config.t2p_pin),
        .mode = GPIO_MODE_INPUT,
        /* CDB and T2P are open-drain outputs on the TPS2378 and this
         * board has no external pull-up resistor on either line — the
         * ESP32's internal pull-up is the only thing holding them HIGH
         * when the TPS2378 isn't actively pulling low. Without this, both
         * pins would float. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* One-time, safety-critical init: without CDB/T2P actually configured
     * as inputs, the whole PoE/AUX gate can't function at all, so a clean
     * reboot (ESP_ERROR_CHECK's default abort+restart) is the safer
     * outcome than silently continuing with unconfigured pins. */
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
    default:                    return 0.0f; /* AUX or NONE: no standardized PoE power budget */
    }
}
