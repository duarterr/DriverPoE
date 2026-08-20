#include "poe_negotiator.h"
#include "poe_luminaire.h"
#include "hv9910.h"
#include "voltage_sense.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_bit_defs.h"
#include "esp_log.h"

static const char *TAG = "POE_NEG";

#define DEBOUNCE_SAMPLES        5     /* consecutive stable readings needed to confirm a transition */
#define SAMPLE_PERIOD_MS        20
#define WAIT_BLINK_PERIOD_MS    150
#define WAIT_LOG_PERIOD_MS      5000  /* heartbeat log while stuck in low-power mode */

/* Guaranteed PD power per IEEE 802.3af/at, in watts (standard headline
 * figures, not a live measurement):
 * https://www.fs.com/blog/understanding-poe-standards-and-wattage-21.html */
#define POE_TYPE1_POWER_W       12.95f  /* 802.3af */
#define POE_TYPE2_POWER_W       25.5f   /* 802.3at Type 2 / PoE+ */

static EventGroupHandle_t s_evt;
#define POE_READY_BIT   BIT0

static volatile bool s_is_ready = false;
static volatile poe_source_t s_source = POE_SOURCE_NONE;
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
    return gpio_get_level(PIN_POE_CDB) != 0;
}

static bool read_t2p_aux_or_type2(void)
{
    /* T2P active LOW = either Type-2 classification was observed, or the
     * AUX (>40V) divider is forcing the TPS2378's APD pin high. Either
     * way, it means "safe to operate" — see poe_negotiator.h. */
    return gpio_get_level(PIN_POE_T2P) == 0;
}

/* Reads VBUS fresh and returns the mV value; also used by the "raw"
 * public getter. Returns 0 (treated as "not ok") if the ADC read fails. */
static int read_vbus_mv(void)
{
    voltage_reading_t v = {0};
    if (voltage_sense_read(&v) != ESP_OK) {
        return 0;
    }
    return v.vbus_mv;
}

static void poe_monitor_task(void *arg)
{
    (void)arg;

    debounce_t cdb_deb = { .last_sample = read_cdb_poe_ok(), .confirmed = false, .stable_count = 0 };
    debounce_t t2p_deb = { .last_sample = read_t2p_aux_or_type2(), .confirmed = false, .stable_count = 0 };
    debounce_t vbus_deb = { .last_sample = (read_vbus_mv() >= VBUS_MIN_MV), .confirmed = false, .stable_count = 0 };
    /* Tracks each raw signal's own last logged state, independently of the
     * combined ready/not-ready verdict below — see the EVENT logs. */
    bool prev_poe_ok = cdb_deb.confirmed;
    bool prev_aux_or_type2 = t2p_deb.confirmed;
    bool prev_vbus_ok = vbus_deb.confirmed;

    TickType_t last_blink = xTaskGetTickCount();
    TickType_t last_wait_log = xTaskGetTickCount();
    bool blink_state = false;

    while (1) {
        bool poe_ok = debounce_update(&cdb_deb, read_cdb_poe_ok());
        bool aux_or_type2 = debounce_update(&t2p_deb, read_t2p_aux_or_type2());

        int vbus_mv = read_vbus_mv();
        s_vbus_mv = vbus_mv;
        bool vbus_ok = debounce_update(&vbus_deb, vbus_mv >= VBUS_MIN_MV);

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
                ESP_LOGI(TAG, "EVENT: VBUS OK — %dmV >= %dmV threshold", vbus_mv, VBUS_MIN_MV);
            } else {
                ESP_LOGW(TAG, "EVENT: VBUS too low — %dmV < %dmV threshold", vbus_mv, VBUS_MIN_MV);
            }
        }

        /* The digital source is tracked independently of VBUS and updated
         * every cycle (not just on the combined ready transition below),
         * so poe_source stays informative even while low-power mode is
         * caused purely by VBUS being too low (CDB/T2P can be confirmed
         * while poe_ready is still 0 — check vbus_ok/vbus_mv to tell the
         * two apart, see poe_negotiator.h). */
        poe_source_t digital_source = POE_SOURCE_NONE;
        if (poe_ok) {
            digital_source = aux_or_type2 ? POE_SOURCE_TYPE2 : POE_SOURCE_TYPE1;
        } else if (aux_or_type2) {
            digital_source = POE_SOURCE_AUX;
        }
        s_source = digital_source;

        bool digital_source_ok = (digital_source != POE_SOURCE_NONE);
        bool new_ready = digital_source_ok && vbus_ok;

        if (new_ready != s_is_ready) {
            s_is_ready = new_ready;
            if (new_ready) {
                xEventGroupSetBits(s_evt, POE_READY_BIT);
                gpio_set_level(PIN_LED_POE_WAIT, 0); /* red LED off */
                ESP_LOGI(TAG, "READY: %s (CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV), %.2fW available",
                         poe_negotiator_source_name(s_source), poe_ok, aux_or_type2, vbus_mv,
                         poe_negotiator_get_available_power_w());
                /* Symmetric with the forced-off cutoff below: if the
                 * driver was on before power was lost (or before this
                 * boot, on a fresh power-up after an outage), come back
                 * on by itself now that power is confirmed safe again --
                 * ramped (default), persist=false since this only
                 * restores existing intent, it doesn't create new intent
                 * (the "on" state is already correctly persisted from
                 * whenever it was actually set). */
                if (hv9910_was_last_on()) {
                    ESP_LOGI(TAG, "Resuming: was ON before, power is ready again");
                    hv9910_enable(HV9910_DEFAULT_RAMP_MS, false);
                }
            } else {
                xEventGroupClearBits(s_evt, POE_READY_BIT);
                /* Immediate, unconditional cut of the LED driver, even if
                 * it was already turned on by a remote command -- instant
                 * (ramp_ms=0, no point fading during a power emergency)
                 * and persist=false, so a real power loss never erases
                 * the "was on" memory the resume above depends on. */
                hv9910_disable(0, false);
                if (digital_source_ok && !vbus_ok) {
                    ESP_LOGW(TAG, "LOW POWER MODE: digital source OK but VBUS too low "
                                  "(CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV < %dmV) — driver forced OFF",
                             poe_ok, aux_or_type2, vbus_mv, VBUS_MIN_MV);
                } else {
                    ESP_LOGW(TAG, "LOW POWER MODE: neither PoE nor AUX confirmed anymore "
                                  "(CDB poe_ok=%d, T2P aux_or_type2=%d, VBUS=%dmV) — driver forced OFF",
                             poe_ok, aux_or_type2, vbus_mv);
                }
            }
        }

        if (!s_is_ready) {
            TickType_t now = xTaskGetTickCount();

            if ((now - last_blink) >= pdMS_TO_TICKS(WAIT_BLINK_PERIOD_MS)) {
                last_blink = now;
                blink_state = !blink_state;
                gpio_set_level(PIN_LED_POE_WAIT, blink_state);
            }

            /* Periodic reminder so the console clearly shows the system
             * is alive and still evaluating, even if no new transition
             * has happened in a while. */
            if ((now - last_wait_log) >= pdMS_TO_TICKS(WAIT_LOG_PERIOD_MS)) {
                last_wait_log = now;
                ESP_LOGI(TAG, "Still in low power mode — CDB poe_ok=%d T2P aux_or_type2=%d VBUS=%dmV (need >=%dmV), "
                              "waiting for PoE or AUX...",
                         poe_ok, aux_or_type2, vbus_mv, VBUS_MIN_MV);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void poe_negotiator_init(void)
{
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_POE_CDB) | (1ULL << PIN_POE_T2P),
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
    gpio_config(&in_cfg);

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_POE_WAIT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(PIN_LED_POE_WAIT, 1); /* on until the first reading is confirmed */

    s_evt = xEventGroupCreate();

    xTaskCreate(poe_monitor_task, "poe_monitor", 3072, NULL, tskIDLE_PRIORITY + 4, NULL);
}

bool poe_negotiator_wait_ready(TickType_t timeout)
{
    EventBits_t bits = xEventGroupWaitBits(s_evt, POE_READY_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & POE_READY_BIT) != 0;
}

bool poe_negotiator_is_ready(void)
{
    return s_is_ready;
}

poe_source_t poe_negotiator_get_source(void)
{
    return s_source;
}

bool poe_negotiator_cdb_confirmed(void)
{
    return s_cdb_confirmed;
}

bool poe_negotiator_t2p_confirmed(void)
{
    return s_t2p_confirmed;
}

bool poe_negotiator_vbus_confirmed(void)
{
    return s_vbus_confirmed;
}

bool poe_negotiator_cdb_raw(void)
{
    return read_cdb_poe_ok();
}

bool poe_negotiator_t2p_raw(void)
{
    return read_t2p_aux_or_type2();
}

bool poe_negotiator_vbus_raw(void)
{
    return read_vbus_mv() >= VBUS_MIN_MV;
}

int poe_negotiator_get_vbus_mv(void)
{
    return s_vbus_mv;
}

const char *poe_negotiator_source_name(poe_source_t source)
{
    switch (source) {
    case POE_SOURCE_TYPE1: return "PoE Type-1/802.3af";
    case POE_SOURCE_TYPE2: return "PoE Type-2/802.3at";
    case POE_SOURCE_AUX:   return "AUX bench supply (not PoE)";
    default:                return "none";
    }
}

const char *poe_negotiator_source_short_name(poe_source_t source)
{
    switch (source) {
    case POE_SOURCE_TYPE1: return "type1";
    case POE_SOURCE_TYPE2: return "type2";
    case POE_SOURCE_AUX:   return "aux";
    default:                return "none";
    }
}

float poe_negotiator_get_available_power_w(void)
{
    switch (s_source) {
    case POE_SOURCE_TYPE1: return POE_TYPE1_POWER_W;
    case POE_SOURCE_TYPE2: return POE_TYPE2_POWER_W;
    default:                return 0.0f; /* AUX or NONE: no standardized PoE power budget */
    }
}
