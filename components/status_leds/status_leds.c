/*
 * See status_leds.h for what each LED means. This file is just the
 * polling loop that reads status via the caller-supplied function
 * pointers (status_leds_config_t::power_ok_fn/driver_on_fn) and turns
 * that into GPIO levels -- no state of its own is exposed to anyone
 * else, and no other component's header is included here on purpose
 * (see status_leds.h).
 */
#include "status_leds.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "STATUS_LED";

#define POLL_PERIOD_MS      150   /* also the blink half-period */
#define TASK_STACK_SIZE     2048
#define TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

/* Board wiring, copied in from status_leds_init()'s config argument. */
static status_leds_config_t s_config;

/* Maps a logical "lit"/"not lit" intent to the electrical level that
 * actually turns this particular LED on, given its configured polarity --
 * every other place in this file works in logical on/off terms and calls
 * this right before touching the GPIO, the same way hv9910.c's
 * shutdown_write()/shutdown_assert_level() do it for SHUTDOWN. */
static int led_level(bool active_high, bool lit)
{
    return (lit == active_high) ? 1 : 0;
}

/* Returns false on failure. Non-fatal by design, unlike the equivalent
 * checks in hv9910.c/tps2378.c: these are purely cosmetic indicator
 * LEDs, not part of the safety-critical PoE/driver path, so a failure
 * here logs loudly and leaves status_leds disabled rather than
 * rebooting the whole device over it. */
static bool configure_led_pin(gpio_num_t pin, bool active_high, bool initial_lit)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d) failed (%s)", (int)pin, esp_err_to_name(err));
        return false;
    }
    gpio_set_level(pin, led_level(active_high, initial_lit));
    return true;
}

static void status_leds_task(void *arg)
{
    (void)arg;
    bool blink_phase = false;

    while (1) {
        blink_phase = !blink_phase;

        /* BLUE: on solid once VBUS is confirmed; blinks while it isn't
         * (covers both "no power at all" and "digital source confirmed
         * but the bus voltage itself is still too low" -- see the
         * power_ok_fn doc comment in status_leds.h). */
        bool vbus_ok = s_config.power_ok_fn ? s_config.power_ok_fn() : false;
        gpio_set_level(s_config.blue_pin, led_level(s_config.blue_active_high, vbus_ok ? true : blink_phase));

        /* RED: on while the LED driver is enabled, off while disabled.
         *
         * TODO(led-fault): once LED string fault detection exists (e.g.
         * cross-checking VLED against an expected forward-voltage range
         * while the driver is enabled), reflect it here as a blink,
         * taking priority over the plain on/off state above -- not
         * implemented yet. */
        bool driver_on = s_config.driver_on_fn ? s_config.driver_on_fn() : false;
        gpio_set_level(s_config.red_pin, led_level(s_config.red_active_high, driver_on));

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void status_leds_init(const status_leds_config_t *config)
{
    /* Copied, not just pointer-retained -- config doesn't need to stay
     * valid after this call returns (see status_leds.h). */
    s_config = *config;

    /* BLUE starts lit immediately (see status_leds.h) -- the very first
     * poll cycle above corrects it to blinking if VBUS actually isn't
     * confirmed yet, which is the common case this early. RED starts
     * unlit, matching hv9910_init()'s forced-disabled state at boot. */
    bool blue_ok = configure_led_pin(s_config.blue_pin, s_config.blue_active_high, true);
    bool red_ok = configure_led_pin(s_config.red_pin, s_config.red_active_high, false);
    if (!blue_ok || !red_ok) {
        ESP_LOGE(TAG, "Indicator LED GPIO setup failed -- status_leds task not started (rest of the firmware is unaffected)");
        return;
    }

    BaseType_t ok = xTaskCreate(status_leds_task, "status_leds", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(status_leds) failed -- indicator LEDs will stay in their initial state");
    }
}
