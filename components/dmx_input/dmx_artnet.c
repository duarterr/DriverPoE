/** @file dmx_artnet.c
 * @brief Art-Net receiver implementation.
 *
 * Handles the three packets that matter for a DMX output node: ArtPoll
 * (answered with ArtPollReply so the fixture is discoverable and named in
 * consoles / DMX-Workshop / QLC+), ArtDmx (the actual level stream), and
 * ArtAddress (remote universe/name programming, gated on
 * dmx_input_config_t.allow_artaddress). Everything else is ignored.
 *
 * Wire notes: the 8-byte ID is "Art-Net\0"; OpCode is little-endian;
 * ProtVer and the ArtDmx Length are big-endian. The 15-bit Port-Address
 * is Net(bits 14-8) : SubNet(bits 7-4) : Universe(bits 3-0).
 */
#include "dmx_artnet.h"
#include "dmx_internal.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "devid.h"
#include "eth_init.h"

static const char *TAG = "DMX_ARTNET";

#define ARTNET_ID           "Art-Net"          /* 8 bytes incl. the NUL */
#define ARTNET_OP_POLL      0x2000
#define ARTNET_OP_POLLREPLY 0x2100
#define ARTNET_OP_DMX       0x5000
#define ARTNET_OP_ADDRESS   0x6000
#define ARTNET_PROT_VER     14

#define ARTNET_DMX_HDR_LEN      18
#define ARTNET_POLL_MIN_LEN     14
#define ARTNET_ADDRESS_LEN      107
#define ARTNET_POLLREPLY_LEN    239

#define ARTNET_STYLE_NODE       0x00
#define ARTNET_PORTTYPE_OUT_DMX 0x80  /* bit7 = output, low bits = DMX512 */

/* Experimental / prototype ESTA manufacturer code (0x7ff0..0x7fff range). */
#define ARTNET_ESTA_MAN        0x7ff0
#define ARTNET_OEM_UNKNOWN     0x00ff

/**
 * @brief Reads the common Art-Net header and returns the opcode.
 * @param buf Datagram.
 * @param len Datagram length.
 * @param op_out Decoded opcode.
 * @return true if the ID matches and the buffer holds an opcode.
 */
static bool artnet_parse_common(const uint8_t *buf, size_t len, uint16_t *op_out)
{
    if (len < 10) {
        return false;
    }
    if (memcmp(buf, ARTNET_ID, 8) != 0) {
        return false;
    }
    *op_out = dmx_get_u16_le(buf + 8);
    return true;
}

/**
 * @brief Builds and sends an ArtPollReply. Sent to the interface's directed
 * broadcast (per spec), falling back to @p fallback if the IP isn't known yet.
 * @param sock Socket fd.
 * @param fallback Destination to use when the directed broadcast can't be computed.
 * @return None.
 */
static void send_poll_reply(int sock, const struct sockaddr_in *fallback)
{
    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);

    uint8_t pkt[ARTNET_POLLREPLY_LEN];
    memset(pkt, 0, sizeof(pkt));

    memcpy(pkt, ARTNET_ID, 8);
    pkt[8] = (uint8_t)(ARTNET_OP_POLLREPLY & 0xff);   /* OpCode little-endian */
    pkt[9] = (uint8_t)(ARTNET_OP_POLLREPLY >> 8);

    esp_netif_ip_info_t ip_info = {0};
    uint32_t ip = 0;
    if (eth_get_ip_info(&ip_info)) {
        ip = ip_info.ip.addr;  /* already network order */
    }
    memcpy(pkt + 10, &ip, 4);

    pkt[14] = (uint8_t)(ARTNET_PORT & 0xff);    /* Port, little-endian */
    pkt[15] = (uint8_t)(ARTNET_PORT >> 8);

    pkt[16] = 0;                                /* VersInfoH */
    pkt[17] = 1;                                /* VersInfoL */

    uint16_t addr = cfg.artnet_port_address & 0x7fff;
    pkt[18] = (uint8_t)((addr >> 8) & 0x7f);    /* NetSwitch (bits 14-8) */
    pkt[19] = (uint8_t)((addr >> 4) & 0x0f);    /* SubSwitch (bits 7-4) */

    pkt[20] = (uint8_t)(ARTNET_OEM_UNKNOWN >> 8);
    pkt[21] = (uint8_t)(ARTNET_OEM_UNKNOWN & 0xff);

    pkt[23] = 0;                                /* Status1 */
    pkt[24] = (uint8_t)(ARTNET_ESTA_MAN & 0xff);/* EstaManLo */
    pkt[25] = (uint8_t)(ARTNET_ESTA_MAN >> 8);  /* EstaManHi */

    strncpy((char *)pkt + 26, devid_get_serial(), 17);   /* ShortName[18] @26 */
    snprintf((char *)pkt + 44, 64, "%s DMX fixture (%s)", /* LongName[64] @44 */
             devid_get_model_prefix(), devid_get_serial());
    snprintf((char *)pkt + 108, 64, "#0001 [0000] Art-Net/sACN DMX input OK"); /* NodeReport[64] @108 */

    pkt[172] = 0;                               /* NumPortsHi */
    pkt[173] = 1;                               /* NumPortsLo */
    pkt[174] = ARTNET_PORTTYPE_OUT_DMX;         /* PortTypes[0] */
    pkt[182] = cfg.layer_enabled ? 0x80 : 0x00; /* GoodOutput[0]: bit7 = data transmitted */
    pkt[190] = (uint8_t)(addr & 0x0f);          /* SwOut[0] (bits 3-0) */

    pkt[200] = ARTNET_STYLE_NODE;               /* Style */
    memcpy(pkt + 201, devid_get_mac(), 6);      /* MAC[6] */
    memcpy(pkt + 207, &ip, 4);                  /* BindIp[4] */
    pkt[211] = 1;                               /* BindIndex */
    pkt[212] = 0x0e;                            /* Status2: Art-Net 3/4 capable, DHCP capable, DHCP used */

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ARTNET_PORT);
    if (ip != 0 && ip_info.netmask.addr != 0) {
        dst.sin_addr.s_addr = ip_info.ip.addr | ~ip_info.netmask.addr;  /* directed broadcast */
    } else if (fallback != NULL) {
        dst.sin_addr = fallback->sin_addr;
        dst.sin_port = fallback->sin_port;
    } else {
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }

    if (sendto(sock, pkt, sizeof(pkt), 0, (const struct sockaddr *)&dst, sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "ArtPollReply sendto() failed: errno %d", errno);
    }
}

/**
 * @brief Handles ArtDmx: filters by universe, hands the slots to the merge engine.
 * @param buf Datagram.
 * @param len Datagram length.
 * @param src_ip Source IPv4 (host order).
 * @return None.
 */
static void handle_dmx(const uint8_t *buf, size_t len, uint32_t src_ip)
{
    if (len < ARTNET_DMX_HDR_LEN) {
        return;
    }
    uint16_t data_len = dmx_get_u16_be(buf + 16);
    if (data_len < 2 || data_len > 512 || len < (size_t)ARTNET_DMX_HDR_LEN + data_len) {
        return;
    }

    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);
    if (!cfg.layer_enabled || !(cfg.proto_mask & DMX_PROTO_ARTNET)) {
        return;
    }

    uint16_t sub_uni = buf[14];
    uint16_t net = buf[15];
    uint16_t port_address = (uint16_t)(((net & 0x7f) << 8) | (sub_uni & 0xff));
    if (port_address != (cfg.artnet_port_address & 0x7fff)) {
        return;
    }

    dmx_frame_t frame = {
        .kind = DMX_FRAME_ARTNET,
        .src_ip = src_ip,
        .priority = 100,
        .terminated = false,
        .nslots = data_len,
        .slots = buf + ARTNET_DMX_HDR_LEN,
    };
    /* Art-Net has no CID: identify a source purely by its IP. */
    memcpy(frame.source_id, &src_ip, sizeof(src_ip));
    dmx_merge_submit(&frame);
}

/**
 * @brief Applies one Art-Net "switch" byte (bit7 = "use this value").
 * @param cur Current nibble/value.
 * @param wire Wire byte from ArtAddress.
 * @param mask Value mask (0x0f for nibbles, 0x7f for NetSwitch).
 * @return New value.
 */
static uint8_t apply_switch(uint8_t cur, uint8_t wire, uint8_t mask)
{
    if (wire & 0x80) {
        return wire & mask;
    }
    return cur;
}

/**
 * @brief Handles ArtAddress: programs universe/name, honors AcCancelMerge.
 * @param sock Socket fd (for the follow-up ArtPollReply).
 * @param buf Datagram.
 * @param len Datagram length.
 * @param dst Destination for the ArtPollReply.
 * @return None.
 */
static void handle_address(int sock, const uint8_t *buf, size_t len, const struct sockaddr_in *dst)
{
    if (len < ARTNET_ADDRESS_LEN) {
        return;
    }

    dmx_input_config_t cfg;
    dmx_cfg_snapshot(&cfg);
    if (!cfg.allow_artaddress) {
        ESP_LOGW(TAG, "ArtAddress ignored (allow_artaddress = 0)");
        return;
    }

    uint16_t addr = cfg.artnet_port_address & 0x7fff;
    uint8_t net = (addr >> 8) & 0x7f;
    uint8_t sub = (addr >> 4) & 0x0f;
    uint8_t uni = addr & 0x0f;

    net = apply_switch(net, buf[12], 0x7f);   /* NetSwitch */
    sub = apply_switch(sub, buf[104], 0x0f);  /* SubSwitch */
    uni = apply_switch(uni, buf[100], 0x0f);  /* SwOut[0] */

    cfg.artnet_port_address = (uint16_t)((net << 8) | (sub << 4) | uni);

    uint8_t command = buf[106];
    if (command == 0x01) {  /* AcCancelMerge */
        dmx_merge_cancel();
    }

    if (!dmx_input_set_config(&cfg)) {
        ESP_LOGW(TAG, "ArtAddress produced an invalid config -- not applied");
        return;
    }
    ESP_LOGI(TAG, "ArtAddress: port-address now 0x%04x", cfg.artnet_port_address);
    send_poll_reply(sock, dst);
}

void dmx_artnet_handle(int sock, uint8_t *buf, size_t len, uint32_t src_ip, uint16_t src_port)
{
    uint16_t op;
    if (!artnet_parse_common(buf, len, &op)) {
        return;
    }

    struct sockaddr_in reply_to = {0};
    reply_to.sin_family = AF_INET;
    reply_to.sin_addr.s_addr = htonl(src_ip);
    reply_to.sin_port = htons(src_port ? src_port : ARTNET_PORT);

    switch (op) {
    case ARTNET_OP_DMX:
        handle_dmx(buf, len, src_ip);
        break;
    case ARTNET_OP_POLL:
        if (len >= ARTNET_POLL_MIN_LEN) {
            send_poll_reply(sock, &reply_to);
        }
        break;
    case ARTNET_OP_ADDRESS:
        handle_address(sock, buf, len, &reply_to);
        break;
    default:
        break;
    }
}

int dmx_artnet_open(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ARTNET_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) failed: errno %d", ARTNET_PORT, errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Art-Net listening on UDP:%d", ARTNET_PORT);
    return sock;
}

void dmx_artnet_close(int sock)
{
    if (sock >= 0) {
        close(sock);
    }
}
