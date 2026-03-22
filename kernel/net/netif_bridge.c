/* AIOS v2 — Network Interface Bridge (Phase 11)
 *
 * Decouples NIC drivers (KAOS modules) from the TCP/IP stack (kernel).
 * E1000 IRQ handler enqueues packets via netif_bridge_rx().
 * lwip_poll() drains the queue via netif_bridge_poll_rx(). */

#include "netif_bridge.h"
#include "../../include/string.h"
#include "../../include/kaos/export.h"
#include "../../drivers/serial.h"

static netif_tx_fn         driver_tx = NULL;
static netif_get_mac_fn    driver_get_mac = NULL;
static netif_rx_callback_fn rx_callback = NULL;
static bool                driver_registered = false;

/* ── RX ring buffer (IRQ-safe single-producer, task-consumer) ── */
#define RX_QUEUE_SIZE 64
static struct netbuf *rx_queue[RX_QUEUE_SIZE];
static volatile uint32_t rx_head = 0;  /* IRQ writes here */
static volatile uint32_t rx_tail_q = 0;  /* Task reads here */

void netif_bridge_init(void) {
    driver_tx = NULL;
    driver_get_mac = NULL;
    rx_callback = NULL;
    driver_registered = false;
    rx_head = 0;
    rx_tail_q = 0;
    serial_print("[netif] bridge initialized\n");
}

int netif_bridge_register_driver(netif_tx_fn tx, netif_get_mac_fn get_mac) {
    if (!tx || !get_mac) return -1;
    driver_tx = tx;
    driver_get_mac = get_mac;
    driver_registered = true;

    uint8_t mac[6];
    get_mac(mac);
    serial_printf("[netif] driver registered, MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

/* Called from E1000 IRQ handler — just enqueue, don't process */
void netif_bridge_rx(struct netbuf *buf) {
    uint32_t next = (rx_head + 1) % RX_QUEUE_SIZE;
    if (next == rx_tail_q) {
        /* Queue full — drop packet */
        netbuf_free(buf);
        return;
    }
    rx_queue[rx_head] = buf;
    rx_head = next;
}

/* Called from task context to drain RX queue into lwIP */
void netif_bridge_poll_rx(void) {
    while (rx_tail_q != rx_head) {
        struct netbuf *buf = rx_queue[rx_tail_q];
        rx_tail_q = (rx_tail_q + 1) % RX_QUEUE_SIZE;

        if (rx_callback) {
            rx_callback(buf);
        } else {
            netbuf_free(buf);
        }
    }
}

int netif_bridge_tx(const uint8_t *data, uint16_t len) {
    if (!driver_tx) return -1;
    return driver_tx(data, len);
}

void netif_bridge_get_mac(uint8_t mac[6]) {
    if (driver_get_mac) {
        driver_get_mac(mac);
    } else {
        memset(mac, 0, 6);
    }
}

bool netif_bridge_has_driver(void) {
    return driver_registered;
}

void netif_bridge_set_rx_callback(netif_rx_callback_fn cb) {
    rx_callback = cb;
}

KAOS_EXPORT(netif_bridge_register_driver)
KAOS_EXPORT(netif_bridge_rx)
KAOS_EXPORT(netif_bridge_tx)
KAOS_EXPORT(netif_bridge_get_mac)
