/** @file dmx_input.c
 * @brief DMX input layer core: config/NVS, source tracking + HTP/LTP merge,
 * signal-loss watchdog, and rate-capped actuation into the HV9910 driver.
 *
 * One task (dmx_input_task) owns everything: it runs select() over the
 * Art-Net and sACN sockets, lets those parsers call dmx_merge_submit()
 * synchronously, and on a fixed ~45 Hz tick recomputes the merged level
 * and pushes it to the hv9910 driver (which never writes NVS).
 *
 * Arbitration with the admin channel is implicit: while any DMX source is
 * live, this task keeps re-applying the merged level every tick, so an
 * admin ON/OFF/DIM is overridden within ~22 ms. When every source times
 * out, this task stops actuating (after applying the configured loss
 * behavior once), and admin control takes over again.
 */
#include "dmx_input.h"
#include "dmx_internal.h"
#include "dmx_artnet.h"
#include "dmx_sacn.h"

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"

#include "hv9910.h"
#include "tps2378.h"

static const char *TAG = "DMX_IN";

#define NVS_NAMESPACE       "dmx"
#define NVS_KEY_CFG         "cfg"

#define TASK_STACK_SIZE     5120
#define TASK_PRIORITY       (tskIDLE_PRIORITY + 5)

#define MAX_SOURCES         4
/* Output cap: 22 ms ~= 45 Hz, just above the DMX512 / Art-Net ceiling of
 * ~44 frames/s per universe (Art-Net spec: "not exceeding 44 per second").
 * Sources never send faster than that, so this drops no distinct frame; it
 * only decouples the RX rate from actuation so a burst of datagrams can't
 * overrun hv9910's 8-deep command queue. hv9910 never touches flash, so
 * matching the DMX rate is free. */
#define ACTUATE_INTERVAL_MS 22
#define RX_BUF_SIZE         640           /* ArtDmx 18+512, E1.31 125+1+512 */

/* --------------------------------------------------------------------- */
/* Config                                                                */
/* --------------------------------------------------------------------- */

static SemaphoreHandle_t s_cfg_lock;
static dmx_input_config_t s_cfg;
static volatile bool s_cfg_changed;      /* set by set_config, cleared by the task */
static bool s_started;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;

/**
 * @brief Fills a config with the factory defaults.
 * @param c Destination.
 * @return None.
 */
static void config_defaults(dmx_input_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->layer_enabled = 0;
    c->proto_mask = DMX_PROTO_ARTNET | DMX_PROTO_SACN;
    c->artnet_port_address = 0;
    c->sacn_universe = 1;
    c->dmx_address = 1;
    c->personality = DMX_PERSONALITY_1CH_8BIT;
    c->merge_mode = DMX_MERGE_HTP;
    c->loss_behavior = DMX_LOSS_HOLD;
    c->loss_level = 0;
    c->loss_timeout_ms = 3000;
    c->smoothing_ms = 25;   /* advisory / unused (see dmx_input.h) -- kept for wire compatibility */
    c->allow_artaddress = 1;
}

void dmx_config_pack(const dmx_input_config_t *c, uint8_t *o)
{
    o[0]  = DMX_CFG_LAYOUT_VERSION;
    o[1]  = c->layer_enabled ? 1 : 0;
    o[2]  = c->proto_mask;
    dmx_put_u16_be(o + 3, c->artnet_port_address);
    dmx_put_u16_be(o + 5, c->sacn_universe);
    dmx_put_u16_be(o + 7, c->dmx_address);
    o[9]  = c->personality;
    o[10] = c->merge_mode;
    o[11] = c->loss_behavior;
    o[12] = c->loss_level;
    dmx_put_u16_be(o + 13, c->loss_timeout_ms);
    dmx_put_u16_be(o + 15, c->smoothing_ms);
    o[17] = c->allow_artaddress ? 1 : 0;
}

bool dmx_config_unpack(const uint8_t *in, size_t len, dmx_input_config_t *c)
{
    if (len != DMX_CFG_WIRE_SIZE || in[0] != DMX_CFG_LAYOUT_VERSION) {
        return false;
    }
    memset(c, 0, sizeof(*c));
    c->layer_enabled = in[1] ? 1 : 0;
    c->proto_mask = in[2];
    c->artnet_port_address = dmx_get_u16_be(in + 3);
    c->sacn_universe = dmx_get_u16_be(in + 5);
    c->dmx_address = dmx_get_u16_be(in + 7);
    c->personality = in[9];
    c->merge_mode = in[10];
    c->loss_behavior = in[11];
    c->loss_level = in[12];
    c->loss_timeout_ms = dmx_get_u16_be(in + 13);
    c->smoothing_ms = dmx_get_u16_be(in + 15);
    c->allow_artaddress = in[17] ? 1 : 0;
    return true;
}

/**
 * @brief Clamps a config to valid ranges.
 * @param c Config to sanitize in place.
 * @return true if it was already valid; false if a field had to be clamped.
 */
static bool config_sanitize(dmx_input_config_t *c)
{
    bool ok = true;
    if (c->proto_mask == 0 || (c->proto_mask & ~(DMX_PROTO_ARTNET | DMX_PROTO_SACN))) {
        c->proto_mask = DMX_PROTO_ARTNET | DMX_PROTO_SACN;
        ok = false;
    }
    if (c->artnet_port_address > 0x7fff) { c->artnet_port_address &= 0x7fff; ok = false; }
    if (c->sacn_universe < 1 || c->sacn_universe > 63999) { c->sacn_universe = 1; ok = false; }

    uint8_t channels = (c->personality == DMX_PERSONALITY_2CH_16BIT) ? 2 : 1;
    if (c->personality > DMX_PERSONALITY_2CH_16BIT) { c->personality = DMX_PERSONALITY_1CH_8BIT; channels = 1; ok = false; }
    if (c->dmx_address < 1 || c->dmx_address > (uint16_t)(513 - channels)) {
        c->dmx_address = 1;
        ok = false;
    }
    if (c->merge_mode > DMX_MERGE_LTP) { c->merge_mode = DMX_MERGE_HTP; ok = false; }
    if (c->loss_behavior > DMX_LOSS_TO_LEVEL) { c->loss_behavior = DMX_LOSS_HOLD; ok = false; }
    if (c->loss_level > 100) { c->loss_level = 100; ok = false; }
    if (c->loss_timeout_ms < 500) { c->loss_timeout_ms = 500; ok = false; }
    if (c->loss_timeout_ms > 60000) { c->loss_timeout_ms = 60000; ok = false; }
    if (c->smoothing_ms > 5000) { c->smoothing_ms = 5000; ok = false; }
    return ok;
}

/**
 * @brief Persists the current config to NVS.
 * @return None.
 */
static void config_save(void)
{
    if (!s_nvs_ok) {
        return;
    }
    uint8_t buf[DMX_CFG_WIRE_SIZE];
    dmx_config_pack(&s_cfg, buf);
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_CFG, buf, sizeof(buf));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist DMX config (%s)", esp_err_to_name(err));
    }
}

/**
 * @brief Opens NVS and loads the persisted config, or writes defaults.
 * @return None.
 */
static void config_load(void)
{
    config_defaults(&s_cfg);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s) -- DMX config won't persist", esp_err_to_name(err));
        return;
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed -- DMX config won't persist");
        return;
    }
    s_nvs_ok = true;

    uint8_t buf[DMX_CFG_WIRE_SIZE];
    size_t len = sizeof(buf);
    err = nvs_get_blob(s_nvs, NVS_KEY_CFG, buf, &len);
    if (err == ESP_OK && dmx_config_unpack(buf, len, &s_cfg)) {
        config_sanitize(&s_cfg);
        ESP_LOGI(TAG, "DMX config loaded: %s, port-addr 0x%04x, sACN univ %u, addr %u",
                 s_cfg.layer_enabled ? "enabled" : "disabled",
                 s_cfg.artnet_port_address, s_cfg.sacn_universe, s_cfg.dmx_address);
    } else {
        ESP_LOGI(TAG, "No valid DMX config in NVS -- writing defaults (layer disabled)");
        config_save();
    }
}

void dmx_cfg_snapshot(dmx_input_config_t *out)
{
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_lock);
}

bool dmx_input_get_config(dmx_input_config_t *out)
{
    if (!s_started) {
        return false;
    }
    dmx_cfg_snapshot(out);
    return true;
}

bool dmx_input_set_config(const dmx_input_config_t *cfg)
{
    if (!s_started) {
        return false;
    }
    dmx_input_config_t next = *cfg;
    config_sanitize(&next);

    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_cfg = next;
    config_save();
    xSemaphoreGive(s_cfg_lock);

    s_cfg_changed = true;
    ESP_LOGI(TAG, "DMX config updated: %s, port-addr 0x%04x, sACN univ %u, addr %u, personality %u",
             next.layer_enabled ? "enabled" : "disabled",
             next.artnet_port_address, next.sacn_universe, next.dmx_address, next.personality);
    return true;
}

/* --------------------------------------------------------------------- */
/* Source tracking + merge                                               */
/* --------------------------------------------------------------------- */

typedef struct {
    bool     in_use;
    dmx_frame_kind_t kind;
    uint8_t  source_id[16];
    uint32_t src_ip;
    uint8_t  priority;
    uint8_t  last_seq;
    bool     have_seq;
    int64_t  last_seen_us;
    uint8_t  ch[2];              /* the fixture's up-to-2 slots, post-address */
    int64_t  arrived_seq_us;     /* for LTP: when this source last updated */
} dmx_source_t;

static dmx_source_t s_sources[MAX_SOURCES];

/* Live status, updated by the task, read under s_cfg_lock. */
static dmx_input_status_t s_status;

/* Frames-per-second rolling counter. */
static int64_t s_fps_window_us;
static uint16_t s_fps_count;

/* Last percent this layer actuated; -1 = "not currently driving". */
static int s_last_applied_pct = -1;
static int64_t s_last_actuate_us;
static bool s_loss_applied;

/**
 * @brief Finds (or allocates, evicting the oldest) the slot for a source id.
 * @param id 16-byte source identifier.
 * @return Pointer to the slot.
 */
static dmx_source_t *source_slot(const uint8_t id[16])
{
    dmx_source_t *oldest = &s_sources[0];
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].in_use && memcmp(s_sources[i].source_id, id, 16) == 0) {
            return &s_sources[i];
        }
        if (!s_sources[i].in_use) {
            return &s_sources[i];
        }
        if (s_sources[i].last_seen_us < oldest->last_seen_us) {
            oldest = &s_sources[i];
        }
    }
    return oldest;
}

void dmx_merge_submit(const dmx_frame_t *f)
{
    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);

    uint8_t channels = (cfg.personality == DMX_PERSONALITY_2CH_16BIT) ? 2 : 1;
    uint16_t base = cfg.dmx_address - 1;   /* 0-based index into the universe */
    if ((uint32_t)base + channels > f->nslots) {
        return;   /* our channels aren't in this frame */
    }

    int64_t now = esp_timer_get_time();
    dmx_source_t *s = source_slot(f->source_id);

    /* sACN sequence ordering (RFC-style window): discard if this frame is
     * behind by 1..20. Art-Net frames carry sequence==0 -> ordering off. */
    if (f->kind == DMX_FRAME_SACN && f->sequence != 0 && s->in_use && s->have_seq) {
        int8_t diff = (int8_t)(f->sequence - s->last_seq);
        if (diff <= 0 && diff > -20) {
            return;
        }
    }

    if (!s->in_use || memcmp(s->source_id, f->source_id, 16) != 0) {
        memset(s, 0, sizeof(*s));
        memcpy(s->source_id, f->source_id, 16);
    }
    s->in_use = !f->terminated;
    s->kind = f->kind;
    s->src_ip = f->src_ip;
    s->priority = f->priority;
    s->last_seq = f->sequence;
    s->have_seq = (f->sequence != 0);
    s->last_seen_us = now;
    s->arrived_seq_us = now;
    s->ch[0] = f->slots[base];
    s->ch[1] = (channels == 2) ? f->slots[base + 1] : 0;

    if (f->terminated) {
        s->in_use = false;
    }

    /* fps accounting */
    if (now - s_fps_window_us >= 1000000) {
        s_status.fps = (s_fps_window_us == 0) ? 0 : s_fps_count;
        s_fps_count = 0;
        s_fps_window_us = now;
    }
    s_fps_count++;
}

void dmx_merge_cancel(void)
{
    dmx_source_t *newest = NULL;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].in_use && (!newest || s_sources[i].arrived_seq_us > newest->arrived_seq_us)) {
            newest = &s_sources[i];
        }
    }
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (&s_sources[i] != newest) {
            s_sources[i].in_use = false;
        }
    }
    ESP_LOGI(TAG, "merge cancelled -- keeping a single source");
}

/**
 * @brief Maps the merged 8/16-bit level to a 0-100 percent.
 * @param cfg Current config.
 * @param hi Coarse byte (channel 1).
 * @param lo Fine byte (channel 2), or 0 for 8-bit personality.
 * @return Brightness percent 0-100.
 */
static uint8_t level_to_percent(const dmx_input_config_t *cfg, uint8_t hi, uint8_t lo)
{
    if (cfg->personality == DMX_PERSONALITY_2CH_16BIT) {
        uint32_t v = ((uint32_t)hi << 8) | lo;      /* 0..65535 */
        return (uint8_t)((v * 100 + 32767) / 65535);
    }
    return (uint8_t)(((uint32_t)hi * 100 + 127) / 255);
}

/**
 * @brief Recomputes the merged output and actuates the driver (rate-capped).
 * @return None.
 */
static void merge_tick(void)
{
    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);

    int64_t now = esp_timer_get_time();
    int64_t timeout_us = (int64_t)cfg.loss_timeout_ms * 1000;

    /* Prune dead sources; find the highest live priority. */
    int live = 0;
    uint8_t best_prio = 0;
    bool artnet_live = false, sacn_live = false;
    for (int i = 0; i < MAX_SOURCES; i++) {
        dmx_source_t *s = &s_sources[i];
        if (!s->in_use) {
            continue;
        }
        if (now - s->last_seen_us > timeout_us) {
            s->in_use = false;
            continue;
        }
        live++;
        if (s->priority > best_prio) {
            best_prio = s->priority;
        }
        if (s->kind == DMX_FRAME_ARTNET) artnet_live = true;
        else sacn_live = true;
    }

    if (live == 0) {
        /* Signal lost: apply the configured behavior exactly once, then
         * stop driving so the admin channel regains control. */
        if (!s_loss_applied && s_last_applied_pct >= 0) {
            if (cfg.loss_behavior == DMX_LOSS_TO_BLACK) {
                hv9910_disable();
            } else if (cfg.loss_behavior == DMX_LOSS_TO_LEVEL) {
                if (cfg.loss_level == 0) {
                    hv9910_disable();
                } else if (hv9910_is_enabled()) {
                    hv9910_set_dim(cfg.loss_level);
                } else {
                    hv9910_enable_at(cfg.loss_level);
                }
            } /* DMX_LOSS_HOLD: leave the driver where it is */
            ESP_LOGI(TAG, "DMX signal lost -- loss behavior %u applied, releasing control", cfg.loss_behavior);
        }
        s_loss_applied = true;
        s_last_applied_pct = -1;

        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
        s_status.active_source = DMX_SOURCE_NONE;
        s_status.layer_enabled = cfg.layer_enabled;
        s_status.artnet_port_address = cfg.artnet_port_address;
        s_status.sacn_universe = cfg.sacn_universe;
        s_status.dmx_address = cfg.dmx_address;
        s_status.personality = cfg.personality;
        s_status.proto_mask = cfg.proto_mask;
        xSemaphoreGive(s_cfg_lock);
        return;
    }
    s_loss_applied = false;

    /* HTP / LTP merge across the sources at the winning priority. */
    uint8_t out_hi = 0, out_lo = 0;
    int64_t newest_us = 0;
    for (int i = 0; i < MAX_SOURCES; i++) {
        dmx_source_t *s = &s_sources[i];
        if (!s->in_use || s->priority != best_prio) {
            continue;
        }
        if (cfg.merge_mode == DMX_MERGE_LTP) {
            if (s->arrived_seq_us >= newest_us) {
                newest_us = s->arrived_seq_us;
                out_hi = s->ch[0];
                out_lo = s->ch[1];
            }
        } else {  /* HTP */
            uint16_t cur = ((uint16_t)out_hi << 8) | out_lo;
            uint16_t cand = ((uint16_t)s->ch[0] << 8) | s->ch[1];
            if (cand > cur) {
                out_hi = s->ch[0];
                out_lo = s->ch[1];
            }
        }
    }

    uint8_t pct = level_to_percent(&cfg, out_hi, out_lo);

    /* Rate-cap actuation and skip no-op repeats. */
    bool due = (now - s_last_actuate_us) >= (ACTUATE_INTERVAL_MS * 1000);
    if ((pct != s_last_applied_pct || s_last_applied_pct < 0) && due) {
        if (tps2378_is_ready()) {
            if (hv9910_is_enabled()) {
                /* set_dim follows the level: pct 0 drives PWMD low (an
                 * RC-filtered LD alone cannot extinguish the HV9910). */
                hv9910_set_dim(pct);
            } else if (pct > 0) {
                hv9910_enable_at(pct);
            }
            /* pct == 0 with the driver off: stay dark, nothing to do. */
            s_last_applied_pct = pct;
            s_last_actuate_us = now;
        }
        /* If power isn't ready we just don't actuate; we'll catch up when it returns. */
    }

    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_status.layer_enabled = cfg.layer_enabled;
    s_status.active_source = artnet_live && sacn_live ? DMX_SOURCE_BOTH
                             : artnet_live ? DMX_SOURCE_ARTNET
                             : sacn_live ? DMX_SOURCE_SACN : DMX_SOURCE_NONE;
    s_status.merged_level_pct = (s_last_applied_pct >= 0) ? (uint8_t)s_last_applied_pct : pct;
    s_status.artnet_port_address = cfg.artnet_port_address;
    s_status.sacn_universe = cfg.sacn_universe;
    s_status.dmx_address = cfg.dmx_address;
    s_status.personality = cfg.personality;
    s_status.proto_mask = cfg.proto_mask;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].in_use) {
            s_status.last_src_ip = s_sources[i].src_ip;
            break;
        }
    }
    xSemaphoreGive(s_cfg_lock);
}

bool dmx_input_get_status(dmx_input_status_t *out)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_cfg_lock);
    return true;
}

/* --------------------------------------------------------------------- */
/* Task                                                                  */
/* --------------------------------------------------------------------- */

static void drop_all_sources(void)
{
    memset(s_sources, 0, sizeof(s_sources));
    s_last_applied_pct = -1;
    s_loss_applied = true;
}

static void dmx_input_task(void *arg)
{
    (void)arg;

    int artnet_sock = dmx_artnet_open();
    int sacn_sock = dmx_sacn_open();
    if (artnet_sock < 0 && sacn_sock < 0) {
        ESP_LOGE(TAG, "no DMX socket could be opened -- task exiting");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t rx[RX_BUF_SIZE];
    int64_t next_tick_us = esp_timer_get_time();
    int rejoin_divider = 0;

    while (1) {
        if (s_cfg_changed) {
            s_cfg_changed = false;
            dmx_input_config_t cfg;
            dmx_cfg_snapshot(&cfg);
            if (!cfg.layer_enabled) {
                drop_all_sources();
            }
            dmx_sacn_rejoin(sacn_sock);
        }
        /* Periodically retry the multicast join: at open() time DHCP may
         * not have finished, so the first attempt often no-ops. Cheap when
         * already joined. ~2 s cadence. */
        if (++rejoin_divider >= (2000 / ACTUATE_INTERVAL_MS)) {
            rejoin_divider = 0;
            dmx_sacn_rejoin(sacn_sock);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (artnet_sock >= 0) { FD_SET(artnet_sock, &rfds); if (artnet_sock > maxfd) maxfd = artnet_sock; }
        if (sacn_sock >= 0)   { FD_SET(sacn_sock, &rfds);   if (sacn_sock > maxfd) maxfd = sacn_sock; }

        int64_t now = esp_timer_get_time();
        int64_t wait_us = next_tick_us - now;
        if (wait_us < 0) wait_us = 0;
        struct timeval tv = { .tv_sec = wait_us / 1000000, .tv_usec = wait_us % 1000000 };

        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n > 0) {
            for (int pass = 0; pass < 2; pass++) {
                int sock = (pass == 0) ? artnet_sock : sacn_sock;
                if (sock < 0 || !FD_ISSET(sock, &rfds)) {
                    continue;
                }
                struct sockaddr_in src;
                socklen_t sl = sizeof(src);
                int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &sl);
                if (len <= 0) {
                    continue;
                }
                uint32_t src_ip = ntohl(src.sin_addr.s_addr);
                uint16_t src_port = ntohs(src.sin_port);
                if (pass == 0) {
                    dmx_artnet_handle(sock, rx, (size_t)len, src_ip, src_port);
                } else {
                    dmx_sacn_handle(rx, (size_t)len, src_ip);
                }
            }
        }

        now = esp_timer_get_time();
        if (now >= next_tick_us) {
            merge_tick();
            /* keep a steady cadence even if we ran long */
            next_tick_us += ACTUATE_INTERVAL_MS * 1000;
            if (next_tick_us < now) {
                next_tick_us = now + ACTUATE_INTERVAL_MS * 1000;
            }
        }
    }
}

void dmx_input_start(void)
{
    if (s_started) {
        return;
    }
    s_cfg_lock = xSemaphoreCreateMutex();
    if (s_cfg_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed -- DMX layer disabled");
        return;
    }

    config_load();
    drop_all_sources();
    s_started = true;

    BaseType_t ok = xTaskCreate(dmx_input_task, "dmx_input", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(dmx_input) failed -- DMX layer will not run");
        s_started = false;
        return;
    }
}
