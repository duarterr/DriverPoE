/** @file dmx_sacn.c
 * @brief sACN (ANSI E1.31) receiver implementation.
 *
 * Only the E1.31 *data* packet (Root vector 0x00000004, Framing vector
 * 0x00000002, DMP vector 0x02) is handled. Synchronization and universe
 * discovery packets are ignored. The socket binds 0.0.0.0:5568 and joins
 * the multicast group 239.255.<hi>.<lo> for the configured universe;
 * unicast delivery to the same port is also accepted.
 *
 * Per-source identity is the 16-byte CID from the root layer. Priority
 * (0-200, default 100) and the stream_terminated option are passed
 * through to the merge engine; the merge engine owns sequence-number
 * ordering.
 */
#include "dmx_sacn.h"
#include "dmx_internal.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "eth_init.h"

static const char *TAG = "DMX_SACN";

static const uint8_t ACN_PACKET_ID[12] = {
    0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00
};

#define SACN_ROOT_VECTOR_DATA   0x00000004u
#define SACN_FRAME_VECTOR_DATA  0x00000002u
#define SACN_DMP_VECTOR_SET     0x02u

#define SACN_OFF_ACN_ID         4
#define SACN_OFF_ROOT_VECTOR    18
#define SACN_OFF_CID            22
#define SACN_OFF_FRAME_VECTOR   40
#define SACN_OFF_PRIORITY       108
#define SACN_OFF_SEQUENCE       111
#define SACN_OFF_OPTIONS        112
#define SACN_OFF_UNIVERSE       113
#define SACN_OFF_DMP_VECTOR     117
#define SACN_OFF_PROP_COUNT     123
#define SACN_OFF_PROP_VALUES    125   /* [0] = DMX start code */

#define SACN_MIN_LEN            126
#define SACN_OPT_TERMINATED     0x40
#define SACN_OPT_PREVIEW        0x80

static uint16_t s_joined_universe;  /* 0 = not joined */

/**
 * @brief Reads a big-endian uint32.
 * @param p Source.
 * @return Value.
 */
static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/**
 * @brief Fills an ip_mreq for a given sACN universe.
 * @param universe Universe number.
 * @param mreq Destination.
 * @return None.
 */
static void universe_mreq(uint16_t universe, struct ip_mreq *mreq)
{
    memset(mreq, 0, sizeof(*mreq));
    uint32_t group = (239u << 24) | (255u << 16) | (((uint32_t)(universe >> 8) & 0xff) << 8) |
                     ((uint32_t)universe & 0xff);
    mreq->imr_multiaddr.s_addr = htonl(group);

    esp_netif_ip_info_t ip_info;
    if (eth_get_ip_info(&ip_info)) {
        mreq->imr_interface.s_addr = ip_info.ip.addr;
    } else {
        mreq->imr_interface.s_addr = htonl(INADDR_ANY);
    }
}

/**
 * @brief Leaves the previously joined group (if any) and joins the one for
 * the currently configured universe.
 * @param sock Socket fd.
 * @return None.
 */
static void sync_membership(int sock)
{
    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);
    uint16_t want = ((cfg.proto_mask & DMX_PROTO_SACN) && cfg.layer_enabled) ? cfg.sacn_universe : 0;

    if (want == s_joined_universe) {
        return;
    }

    if (s_joined_universe != 0) {
        struct ip_mreq mreq;
        universe_mreq(s_joined_universe, &mreq);
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    }

    if (want == 0) {
        s_joined_universe = 0;
        return;
    }

    struct ip_mreq mreq;
    universe_mreq(want, &mreq);
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        /* Usually just "no IP yet" -- leave s_joined_universe at 0 so the
         * caller retries on the next tick. */
        ESP_LOGD(TAG, "IP_ADD_MEMBERSHIP for universe %u not ready yet: errno %d", want, errno);
        s_joined_universe = 0;
        return;
    }
    ESP_LOGI(TAG, "sACN joined multicast group for universe %u", want);
    s_joined_universe = want;
}

void dmx_sacn_handle(uint8_t *buf, size_t len, uint32_t src_ip)
{
    if (len < SACN_MIN_LEN) {
        return;
    }
    if (memcmp(buf + SACN_OFF_ACN_ID, ACN_PACKET_ID, sizeof(ACN_PACKET_ID)) != 0) {
        return;
    }
    if (get_u32_be(buf + SACN_OFF_ROOT_VECTOR) != SACN_ROOT_VECTOR_DATA) {
        return;
    }
    if (get_u32_be(buf + SACN_OFF_FRAME_VECTOR) != SACN_FRAME_VECTOR_DATA) {
        return;
    }
    if (buf[SACN_OFF_DMP_VECTOR] != SACN_DMP_VECTOR_SET) {
        return;
    }

    uint8_t options = buf[SACN_OFF_OPTIONS];
    if (options & SACN_OPT_PREVIEW) {
        return;  /* preview data is not for live output */
    }

    uint16_t universe = dmx_get_u16_be(buf + SACN_OFF_UNIVERSE);

    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);
    if (!cfg.layer_enabled || !(cfg.proto_mask & DMX_PROTO_SACN)) {
        return;
    }
    if (universe != cfg.sacn_universe) {
        return;
    }

    uint16_t prop_count = dmx_get_u16_be(buf + SACN_OFF_PROP_COUNT);
    if (prop_count < 1) {
        return;
    }
    uint16_t slot_count = prop_count - 1;  /* minus the start-code byte */
    if (slot_count > 512) {
        slot_count = 512;
    }
    if (len < (size_t)SACN_OFF_PROP_VALUES + 1 + slot_count) {
        slot_count = (uint16_t)(len - SACN_OFF_PROP_VALUES - 1);
    }
    if (buf[SACN_OFF_PROP_VALUES] != 0x00) {
        return;  /* not a DMX (null start code) frame */
    }

    dmx_frame_t frame = {
        .kind = DMX_FRAME_SACN,
        .src_ip = src_ip,
        .priority = buf[SACN_OFF_PRIORITY],
        .sequence = buf[SACN_OFF_SEQUENCE],
        .terminated = (options & SACN_OPT_TERMINATED) != 0,
        .nslots = slot_count,
        .slots = buf + SACN_OFF_PROP_VALUES + 1,
    };
    memcpy(frame.source_id, buf + SACN_OFF_CID, 16);
    dmx_merge_submit(&frame);
}

int dmx_sacn_open(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SACN_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) failed: errno %d", SACN_PORT, errno);
        close(sock);
        return -1;
    }

    s_joined_universe = 0;
    sync_membership(sock);

    ESP_LOGI(TAG, "sACN listening on UDP:%d", SACN_PORT);
    return sock;
}

void dmx_sacn_close(int sock)
{
    if (sock < 0) {
        return;
    }
    if (s_joined_universe != 0) {
        struct ip_mreq mreq;
        universe_mreq(s_joined_universe, &mreq);
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        s_joined_universe = 0;
    }
    close(sock);
}

void dmx_sacn_rejoin(int sock)
{
    if (sock >= 0) {
        sync_membership(sock);
    }
}
