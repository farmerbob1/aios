/* AIOS v2 — lwIP Network Interface Driver (Phase 11c)
 *
 * Bridges lwIP to the E1000 NIC via the netif_bridge abstraction.
 * Converts between lwIP pbufs and our netbufs. */

#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif_bridge.h"
#include "netbuf.h"
#include "../../drivers/serial.h"
#include "../../include/string.h"

/* The global lwIP netif for the E1000 */
struct netif aios_netif;

static err_t aios_netif_linkoutput(struct netif *netif, struct pbuf *p);

/* ── Netif init callback ────────────────────────────────── */

err_t aios_netif_init(struct netif *netif) {
    /* Set interface name */
    netif->name[0] = 'e';
    netif->name[1] = '0';

    /* Set output functions */
    netif->output = etharp_output;       /* IP → ARP resolution → linkoutput */
    netif->linkoutput = aios_netif_linkoutput;  /* Raw Ethernet frame out */

    /* Set MAC address from E1000 driver */
    netif->hwaddr_len = 6;
    netif_bridge_get_mac(netif->hwaddr);

    /* Set MTU and flags */
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_LINK_UP | NETIF_FLAG_ETHERNET;

    serial_printf("[lwip-netif] interface e0 initialized, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
                  netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
    return ERR_OK;
}

/* ── TX: lwIP pbuf → E1000 ──────────────────────────────── */

static err_t aios_netif_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;

    if (p->tot_len > 1536) return ERR_BUF;

    /* Flatten pbuf chain into a contiguous buffer */
    uint8_t buf[1536];
    uint16_t offset = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(buf + offset, q->payload, q->len);
        offset += q->len;
    }

    int rc = netif_bridge_tx(buf, offset);
    return (rc == 0) ? ERR_OK : ERR_IF;
}

/* ── RX: E1000 → lwIP ──────────────────────────────────── */

void aios_netif_input(struct netbuf *nb) {
    if (!nb || nb->len == 0) {
        netbuf_free(nb);
        return;
    }

    /* Allocate a pbuf and copy packet data */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, nb->len, PBUF_POOL);
    if (!p) {
        netbuf_free(nb);
        return;
    }

    /* Copy data into pbuf chain */
    uint16_t offset = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        uint16_t copy_len = q->len;
        if (offset + copy_len > nb->len) copy_len = nb->len - offset;
        memcpy(q->payload, nb->data + offset, copy_len);
        offset += copy_len;
    }

    netbuf_free(nb);

    /* Feed into lwIP — ethernet_input handles ARP and IP */
    if (aios_netif.input(p, &aios_netif) != ERR_OK) {
        pbuf_free(p);
    }
}
