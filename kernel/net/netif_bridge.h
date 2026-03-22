/* AIOS v2 — Network Interface Bridge (Phase 11)
 *
 * Bridges between NIC driver KAOS modules and the kernel TCP/IP stack.
 * The E1000 module registers send/recv callbacks; lwIP calls them. */

#pragma once

#include "../../include/types.h"
#include "netbuf.h"

/* Function pointers that the NIC driver registers */
typedef int  (*netif_tx_fn)(const uint8_t *data, uint16_t len);
typedef void (*netif_get_mac_fn)(uint8_t mac[6]);

void netif_bridge_init(void);

/* Called by NIC driver module to register itself */
int  netif_bridge_register_driver(netif_tx_fn tx, netif_get_mac_fn get_mac);

/* Called by NIC driver IRQ handler to deliver received packet */
void netif_bridge_rx(struct netbuf *buf);

/* Called by TCP/IP stack to transmit a packet */
int  netif_bridge_tx(const uint8_t *data, uint16_t len);

/* Called by TCP/IP stack to get MAC address */
void netif_bridge_get_mac(uint8_t mac[6]);

/* Check if a driver is registered */
bool netif_bridge_has_driver(void);

/* RX callback — set by TCP/IP stack to receive packets */
typedef void (*netif_rx_callback_fn)(struct netbuf *buf);
void netif_bridge_set_rx_callback(netif_rx_callback_fn cb);

/* Poll RX queue — drains queued packets into the RX callback.
 * Must be called from task context (not IRQ). */
void netif_bridge_poll_rx(void);
