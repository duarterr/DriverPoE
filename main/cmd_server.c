#include "cmd_server.h"
#include "hv9910.h"
#include "poe_negotiator.h"
#include "voltage_sense.h"
#include "eth_init.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"

#include "lwip/sockets.h"
#include "lwip/err.h"

static const char *TAG = "CMD_SRV";

#define RX_LINE_MAX   96
#define TX_BUF_MAX    256

static void send_line(int sock, const char *msg)
{
    size_t len = strlen(msg);
    int to_write = (int)len;
    while (to_write > 0) {
        int written = send(sock, msg + (len - to_write), to_write, 0);
        if (written < 0) {
            ESP_LOGE(TAG, "send() failed: errno %d", errno);
            return;
        }
        to_write -= written;
    }
}

static void build_status_line(char *out, size_t out_size)
{
    voltage_reading_t v = {0};
    voltage_sense_read(&v);

    char ip_part[32] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    if (eth_get_ip_info(&ip_info)) {
        snprintf(ip_part, sizeof(ip_part), IPSTR, IP2STR(&ip_info.ip));
    }

    int64_t uptime_s = esp_timer_get_time() / 1000000;

    snprintf(out, out_size,
             "OK STATUS poe_ready=%d poe_source=%s poe_power_w=%.2f "
             "cdb_raw=%d cdb_ok=%d t2p_raw=%d t2p_ok=%d vbus_ok=%d "
             "driver_on=%d dim=%u dim_last=%u "
             "vbus_mv=%d led_voltage_mv=%d eth_ip=%s uptime_s=%lld\n",
             poe_negotiator_is_ready() ? 1 : 0,
             poe_negotiator_source_short_name(poe_negotiator_get_source()),
             poe_negotiator_get_available_power_w(),
             poe_negotiator_cdb_raw() ? 1 : 0,
             poe_negotiator_cdb_confirmed() ? 1 : 0,
             poe_negotiator_t2p_raw() ? 1 : 0,
             poe_negotiator_t2p_confirmed() ? 1 : 0,
             poe_negotiator_vbus_confirmed() ? 1 : 0,
             hv9910_is_enabled() ? 1 : 0,
             hv9910_get_dim_percent(),
             hv9910_get_last_nonzero_percent(),
             v.vbus_mv, v.led_voltage_mv,
             ip_part, uptime_s);
}

static const char *HELP_TEXT =
    "OK HELP commands:\n"
    "  PING              -> OK PONG\n"
    "  STATUS            -> current state (PoE/AUX source, driver, dimming, voltages, IP)\n"
    "  ON                -> enables the HV9910 driver, resuming the last brightness (requires PoE or AUX confirmed)\n"
    "  OFF               -> disables the HV9910 driver\n"
    "  DIM <0-100>       -> sets the brightness (%); DIM 0 also turns the driver off, DIM >0 also turns it on\n"
    "  HELP              -> this message\n";

/* Processes one command line, already stripped of '\r'/'\n'. Writes the
 * response into 'resp' (buffer of size resp_size). */
static void handle_line(char *line, char *resp, size_t resp_size)
{
    /* trim leading whitespace */
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if (line[0] == '\0') {
        resp[0] = '\0';
        return;
    }

    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t", &saveptr);
    if (cmd == NULL) {
        snprintf(resp, resp_size, "ERR EMPTY\n");
        return;
    }

    if (strcasecmp(cmd, "PING") == 0) {
        snprintf(resp, resp_size, "OK PONG\n");

    } else if (strcasecmp(cmd, "HELP") == 0) {
        snprintf(resp, resp_size, "%s", HELP_TEXT);

    } else if (strcasecmp(cmd, "STATUS") == 0) {
        build_status_line(resp, resp_size);

    } else if (strcasecmp(cmd, "ON") == 0) {
        if (!poe_negotiator_is_ready()) {
            snprintf(resp, resp_size, "ERR POE_NOT_READY\n");
        } else {
            hv9910_enable();
            snprintf(resp, resp_size, "OK ON\n");
        }

    } else if (strcasecmp(cmd, "OFF") == 0) {
        hv9910_disable();
        snprintf(resp, resp_size, "OK OFF\n");

    } else if (strcasecmp(cmd, "DIM") == 0) {
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (arg == NULL) {
            snprintf(resp, resp_size, "ERR MISSING_ARG\n");
        } else {
            char *endptr = NULL;
            long val = strtol(arg, &endptr, 10);
            if (endptr == arg || val < 0 || val > 100) {
                snprintf(resp, resp_size, "ERR BAD_ARG\n");
            } else {
                hv9910_set_dim_percent((uint8_t)val);

                /* DIM 0 while the driver is on turns it off; DIM > 0
                 * while the driver is off turns it on, subject to the
                 * same PoE/AUX gate as the ON command (if the gate
                 * refuses, the brightness is still recorded/persisted so
                 * it's ready to apply whenever the driver is allowed to
                 * turn on). */
                if (val == 0) {
                    if (hv9910_is_enabled()) {
                        hv9910_disable();
                    }
                } else if (!hv9910_is_enabled() && poe_negotiator_is_ready()) {
                    hv9910_enable();
                }

                snprintf(resp, resp_size, "OK DIM %ld\n", val);
            }
        }

    } else {
        snprintf(resp, resp_size, "ERR UNKNOWN_CMD\n");
    }
}

static void handle_connection(int sock)
{
    char rx_buf[RX_LINE_MAX];
    size_t rx_len = 0;
    char tx_buf[TX_BUF_MAX];

    while (1) {
        char c;
        int len = recv(sock, &c, 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "recv() failed: errno %d", errno);
            break;
        }
        if (len == 0) {
            ESP_LOGI(TAG, "Client disconnected");
            break;
        }

        if (c == '\n') {
            if (rx_len > 0 && rx_buf[rx_len - 1] == '\r') {
                rx_len--;
            }
            rx_buf[rx_len] = '\0';
            handle_line(rx_buf, tx_buf, sizeof(tx_buf));
            if (tx_buf[0] != '\0') {
                send_line(sock, tx_buf);
            }
            rx_len = 0;
        } else if (rx_len < sizeof(rx_buf) - 1) {
            rx_buf[rx_len++] = c;
        } else {
            /* line too long: discard until the next '\n' */
            rx_len = 0;
            send_line(sock, "ERR LINE_TOO_LONG\n");
        }
    }
}

static void cmd_server_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Command server listening on port %u", port);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "New client connected");
        handle_connection(sock);
        shutdown(sock, 0);
        close(sock);
    }
}

void cmd_server_start(uint16_t port)
{
    xTaskCreate(cmd_server_task, "cmd_server", 4096, (void *)(uintptr_t)port, tskIDLE_PRIORITY + 3, NULL);
}
