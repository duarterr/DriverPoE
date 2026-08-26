/** @file status_leds.c
 * @brief Polling loop that drives the power/driver indicator LEDs.
 */
#include "status_leds.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "STATUS_LED";

#define POLL_PERIOD_MS      150
#define TASK_STACK_SIZE      2048
#define TASK_PRIORITY        (tskIDLE_PRIORITY + 1)

static status_leds_config_t s_config;

/**
 * @brief Converts a logical lit/unlit intent to the electrical level for a given polarity.
 * @param active_high LED polarity.
 * @param lit Desired logical state.
 * @return GPIO level to write.
 */
static int led_level(bool active_high, bool lit)
{
    return (lit == active_high) ? 1 : 0;
}

/**
 * @brief Configures one indicator LED's GPIO and sets its initial state.
 * @param pin GPIO to configure.
 * @param active_high LED polarity.
 * @param initial_lit Initial logical state.
 * @return true on success; false if GPIO configuration failed.
 */
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

/**
 * @brief Periodically samples power/driver status and updates the LEDs.
 * @param arg Unused.
 * @return Never returns.
 */
static void status_leds_task(void *arg)
{
    (void)arg;
    bool blink_phase = false;

    while (1) {
        blink_phase = !blink_phase;

        bool vbus_ok = s_config.power_ok_fn ? s_config.power_ok_fn() : false;
        gpio_set_level(s_config.blue_pin, led_level(s_config.blue_active_high, vbus_ok ? true : blink_phase));

        bool driver_on = s_config.driver_on_fn ? s_config.driver_on_fn() : false;
        gpio_set_level(s_config.red_pin, led_level(s_config.red_active_high, driver_on));

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void status_leds_init(const status_leds_config_t *config)
{
    s_config = *config;

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
