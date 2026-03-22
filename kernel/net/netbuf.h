/* AIOS v2 — Network Packet Buffer Pool (Phase 11) */

#pragma once

#include "../../include/types.h"

#define NETBUF_DATA_SIZE  1536  /* MTU 1500 + Ethernet header room */
#define NETBUF_POOL_SIZE  64

struct netbuf {
    uint8_t  data[NETBUF_DATA_SIZE];
    uint16_t len;
    uint16_t flags;
    struct netbuf *next;
};

void           netbuf_init(void);
struct netbuf* netbuf_alloc(void);
void           netbuf_free(struct netbuf *buf);
int            netbuf_free_count(void);
