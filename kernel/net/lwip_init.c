/* AIOS v2 — lwIP Stack Initialization (Phase 11c)
 *
 * Initializes lwIP in NO_SYS=1 mode, creates the network interface,
 * starts DHCP, and runs a timer task for lwIP timeouts.
 *
 * lwip_poll() is the central function — drains RX queue + checks timeouts.
 * Called by the timer task AND by Lua net blocking functions. */

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"
#include "netif_bridge.h"
#include "../../include/types.h"
#include "../../include/boot_info.h"
#include "../../drivers/serial.h"

/* Provided by lwip_netif.c */
extern struct netif aios_netif;
extern err_t aios_netif_init(struct netif *netif);
extern void  aios_netif_input(struct netbuf *nb);

/* Random seed (defined in sys_arch.c, used by LWIP_RAND in cc.h) */
extern unsigned int lwip_rand_seed;

extern uint32_t timer_get_ticks(void);

/* Scheduler API */
extern int  task_create(const char *name, void (*entry)(void), int priority);
extern void task_sleep(uint32_t ms);

/* ── lwip_poll: central processing function ─────────────── */

static volatile bool lwip_poll_active = false;

void lwip_poll(void) {
    /* Prevent reentrant calls (timer task vs Lua task) */
    if (lwip_poll_active) return;
    lwip_poll_active = true;

    /* Drain RX queue — process received packets through lwIP */
    netif_bridge_poll_rx();

    /* Process lwIP timers (DHCP, ARP, TCP retransmit, etc.) */
    sys_check_timeouts();

    lwip_poll_active = false;
}

/* DHCP status callback */
static void netif_status_callback(struct netif *nif) {
    if (nif->ip_addr.addr != 0) {
        uint32_t ip = nif->ip_addr.addr;
        uint32_t mask = nif->netmask.addr;
        uint32_t gw = nif->gw.addr;
        serial_printf("[lwip] IP: %u.%u.%u.%u  mask: %u.%u.%u.%u  gw: %u.%u.%u.%u\n",
                      ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
                      mask & 0xFF, (mask >> 8) & 0xFF, (mask >> 16) & 0xFF, (mask >> 24) & 0xFF,
                      gw & 0xFF, (gw >> 8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
    }
}

/* ── Timer task ─────────────────────────────────────────── */

static void lwip_timer_task(void) {
    serial_print("[lwip] timer task started\n");
    while (1) {
        lwip_poll();
        task_sleep(4);  /* ~250 Hz */
    }
}

/* ── Stack Init ─────────────────────────────────────────── */

init_result_t lwip_stack_init(void) {
    serial_print("[lwip] initializing stack...\n");

    /* Seed random from timer */
    lwip_rand_seed = timer_get_ticks() ^ 0xDEADBEEF;

    /* Initialize lwIP */
    lwip_init();

    /* Check if NIC driver is available */
    if (!netif_bridge_has_driver()) {
        serial_print("[lwip] WARNING: no NIC driver registered\n");
        return INIT_WARN;
    }

    /* Set up RX callback so poll_rx delivers packets to lwIP */
    netif_bridge_set_rx_callback((void *)aios_netif_input);

    /* Add network interface */
    ip4_addr_t ipaddr, netmask, gw;
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    netif_add(&aios_netif, &ipaddr, &netmask, &gw, NULL,
              aios_netif_init, ethernet_input);
    netif_set_default(&aios_netif);
    netif_set_up(&aios_netif);

    /* Set status callback for DHCP */
    netif_set_status_callback(&aios_netif, netif_status_callback);

    /* Start DHCP */
    serial_print("[lwip] starting DHCP...\n");
    dhcp_start(&aios_netif);

    /* Start timer task for lwIP timeouts */
    task_create("lwip_timer", lwip_timer_task, 1);  /* PRIORITY_NORMAL */

    serial_printf("[lwip] stack ready (lwIP %s)\n", LWIP_VERSION_STRING);

    /* Start SNTP for system clock */
    extern void sysclock_init(void);
    sysclock_init();

    return INIT_OK;
}
