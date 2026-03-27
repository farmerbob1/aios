/* AIOS v2 — lwIP Configuration (lwipopts.h)
 * NO_SYS=1 mode: no OS threading, raw/callback API only.
 * DHCP, DNS, TCP, UDP, ICMP, ARP enabled. */

#ifndef AIOS_LWIPOPTS_H
#define AIOS_LWIPOPTS_H

/* ── Core mode ──────────────────────────────────────────── */
#define NO_SYS                  1
#define NO_SYS_NO_TIMERS        0   /* We do want timers */
#define LWIP_SOCKET             0   /* No BSD socket API */
#define LWIP_NETCONN            0   /* No sequential API (NO_SYS=1) */
#define LWIP_NETIF_API          0

/* ── IPv4 ───────────────────────────────────────────────── */
#define LWIP_IPV4               1
#define LWIP_IPV6               0

/* ── Protocols ──────────────────────────────────────────── */
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_ICMP               1
#define LWIP_ARP                1
#define LWIP_DHCP               1
#define LWIP_DNS                1
#define LWIP_IGMP               0
#define LWIP_RAW                1
#define LWIP_AUTOIP             0

/* ── Memory ─────────────────────────────────────────────── */
/* Use heap allocator (kmalloc/kfree) instead of lwIP's pool allocator */
#define MEM_LIBC_MALLOC         1
#define MEMP_MEM_MALLOC         1
#define MEM_SIZE                (128 * 1024)  /* 128KB lwIP internal heap */

extern void *kmalloc(unsigned int size);
extern void  kfree(void *ptr);
static inline void *lwip_calloc(unsigned int count, unsigned int size) {
    unsigned int total = count * size;
    void *p = kmalloc(total);
    if (p) {
        extern void *memset(void *, int, unsigned int);
        memset(p, 0, total);
    }
    return p;
}
#define mem_clib_malloc  kmalloc
#define mem_clib_free    kfree
#define mem_clib_calloc  lwip_calloc

/* ── Pool sizes ─────────────────────────────────────────── */
#define MEMP_NUM_PBUF           16
#define MEMP_NUM_UDP_PCB        4
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG        16
#define MEMP_NUM_SYS_TIMEOUT    12

/* ── Pbuf ───────────────────────────────────────────────── */
#define PBUF_POOL_SIZE          16
#define PBUF_POOL_BUFSIZE       1536

/* ── TCP ────────────────────────────────────────────────── */
#define TCP_MSS                 1460
#define TCP_WND                 (12 * TCP_MSS)
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ         1
#define LWIP_TCP_SACK_OUT       0

/* ── DHCP ───────────────────────────────────────────────── */
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

/* ── DNS ────────────────────────────────────────────────── */
#define LWIP_DNS                1
#define DNS_TABLE_SIZE          4
#define DNS_MAX_NAME_LENGTH     256

/* ── Network interface ──────────────────────────────────── */
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_SINGLE_NETIF          1

/* ── Checksum ───────────────────────────────────────────── */
/* Let software calculate all checksums */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1
#define CHECKSUM_CHECK_ICMP     1

/* ── Debug (disabled for production) ────────────────────── */
#define LWIP_DEBUG              0
#define LWIP_DBG_MIN_LEVEL      LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON       LWIP_DBG_ON

/* ── Error handling ─────────────────────────────────────── */
#define LWIP_PROVIDE_ERRNO      1

/* ── SNTP (Network Time Protocol) ───────────────────────── */
#define LWIP_SNTP                   1
#define SNTP_SERVER_DNS             1
#define SNTP_MAX_SERVERS            2
#define SNTP_UPDATE_DELAY           3600000  /* Re-sync every hour (ms) */

/* SNTP calls this when time is received. Defined in sysclock.c */
extern void aios_set_system_time(unsigned int sec);
#define SNTP_SET_SYSTEM_TIME(sec)   aios_set_system_time(sec)

/* ── Misc ───────────────────────────────────────────────── */
#define LWIP_STATS              0
#define LWIP_STATS_DISPLAY      0
#define LWIP_TIMEVAL_PRIVATE    1
#define LWIP_HAVE_LOOPIF        0
#define LWIP_NETIF_LOOPBACK     0
#define LWIP_ACD                0

/* Lightweight protection (interrupt disable/enable) */
#define SYS_LIGHTWEIGHT_PROT    1

#endif /* AIOS_LWIPOPTS_H */
