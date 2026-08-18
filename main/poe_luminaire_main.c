/*
 * PoE luminaire — minimal firmware.
 *
 * Hardware: ESP32 + IP101G PHY (RMII) + TPS2378 PoE negotiator + HV9910 LED
 * driver. See main/board_pins.h for the full pin map and the sources used
 * to validate each chip's behavior.
 *
 * Boot sequence:
 *   1) hv9910_init() — the very first thing in the firmware: guarantees
 *      the LED driver is off (SHUTDOWN asserted, PWM at 0%) before any
 *      other initialization.
 *   2) voltage_sense_init() — brings up the ADC so VBUS is already
 *      readable before the PoE/AUX gate starts evaluating it.
 *   3) poe_negotiator_init() — starts monitoring the TPS2378's CDB and T2P
 *      pins, cross-checked against the measured VBUS. Until a digital
 *      source (real PoE via CDB, or the bench AUX supply via T2P) is
 *      confirmed AND VBUS backs it up, the system stays in low-power
 *      mode: indicator LED 2 (red) blinks and the LED driver stays forced
 *      OFF (the monitoring task cuts the HV9910 immediately if power is
 *      lost again, even after the driver was already released once). See
 *      main/poe_negotiator.h for the full PoE-vs-AUX-vs-VBUS logic.
 *   4) Only once ready: brings up the PHY/Ethernet, gets an IP, and
 *      starts the TCP command server.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "board_pins.h"
#include "hv9910.h"
#include "poe_negotiator.h"
#include "voltage_sense.h"
#include "eth_init.h"
#include "cmd_server.h"

static const char *TAG = "MAIN";

/* Silences the very verbose ESP-IDF startup logs (bootloader partition
 * table dump, esp_image segment loading, cpu_start, heap_init, spi_flash
 * probe, app_init banner, efuse_init, ...) while keeping our own module
 * logs visible.
 *
 * This only works together with sdkconfig.defaults setting
 * CONFIG_LOG_DEFAULT_LEVEL_WARN (so those IDF-internal INFO logs never
 * print in the first place) and CONFIG_LOG_MAXIMUM_LEVEL_INFO (so INFO is
 * still compiled in and can be re-enabled at runtime for our own tags
 * below). The bootloader's own separate log output is silenced by
 * CONFIG_BOOTLOADER_LOG_LEVEL_WARN. */
static void quiet_boot_noise(void)
{
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("HV9910", ESP_LOG_INFO);
    esp_log_level_set("POE_NEG", ESP_LOG_INFO);
    esp_log_level_set("VOLT_SENSE", ESP_LOG_INFO);
    esp_log_level_set("ETH_INIT", ESP_LOG_INFO);
    esp_log_level_set("CMD_SRV", ESP_LOG_INFO);
}

static void init_status_led(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_STATUS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LED_STATUS, 0);
}

void app_main(void)
{
    quiet_boot_noise();

    /* 1) Safety first: LED driver guaranteed OFF. */
    hv9910_init();

    /* 2) Overall status indicator LED (heartbeat / Ethernet link). */
    init_status_led();

    /* 3) Bring up the ADC before the PoE/AUX gate, since it needs to read
     * VBUS on every monitoring cycle. */
    voltage_sense_init();

    /* 4) Start monitoring the PoE negotiator (CDB/T2P/VBUS) and blink the
     * red LED while doing so. */
    poe_negotiator_init();

    ESP_LOGI(TAG, "Waiting for external power (PoE via TPS2378 CDB, or AUX via T2P, backed by VBUS)...");
    poe_negotiator_wait_ready(portMAX_DELAY);
    ESP_LOGI(TAG, "Ready: %s, %.2fW available. Bringing up Ethernet...",
             poe_negotiator_source_name(poe_negotiator_get_source()),
             poe_negotiator_get_available_power_w());

    /* 5) Only now: initialize PHY/Ethernet and the command server. */
    eth_bringup();
    cmd_server_start(APP_TCP_PORT);

    /* 6) Main loop: only handles the status LED heartbeat. All the
     * control logic runs in the poe_negotiator and cmd_server tasks; the
     * CDB/T2P/VBUS watchdog (inside poe_negotiator) keeps running and cuts
     * the HV9910 at any moment if PoE/AUX power is lost. */
    bool blink_state = false;
    while (1) {
        if (eth_has_ip()) {
            gpio_set_level(PIN_LED_STATUS, 1); /* solid on: network operational */
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            blink_state = !blink_state; /* blinking: waiting for link/IP */
            gpio_set_level(PIN_LED_STATUS, blink_state);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}
