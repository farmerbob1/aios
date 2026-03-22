/* AIOS v2 — Network Packet Buffer Pool (Phase 11)
 *
 * Pre-allocated pool of fixed-size packet buffers for zero-allocation
 * packet handling in the E1000 IRQ path. */

#include "netbuf.h"
#include "../heap.h"
#include "../../include/string.h"
#include "../../include/kaos/export.h"
#include "../../drivers/serial.h"

static struct netbuf *pool = NULL;
static struct netbuf *free_list = NULL;
static int free_count = 0;

void netbuf_init(void) {
    pool = (struct netbuf *)kmalloc(sizeof(struct netbuf) * NETBUF_POOL_SIZE);
    if (!pool) {
        serial_print("[netbuf] ERROR: failed to allocate pool\n");
        return;
    }
    memset(pool, 0, sizeof(struct netbuf) * NETBUF_POOL_SIZE);

    /* Build free list */
    free_list = NULL;
    for (int i = NETBUF_POOL_SIZE - 1; i >= 0; i--) {
        pool[i].next = free_list;
        free_list = &pool[i];
    }
    free_count = NETBUF_POOL_SIZE;

    serial_printf("[netbuf] pool: %d buffers (%u KB)\n",
                  NETBUF_POOL_SIZE,
                  (uint32_t)(sizeof(struct netbuf) * NETBUF_POOL_SIZE / 1024));
}

struct netbuf* netbuf_alloc(void) {
    if (!free_list) return NULL;
    struct netbuf *buf = free_list;
    free_list = buf->next;
    buf->next = NULL;
    buf->len = 0;
    buf->flags = 0;
    free_count--;
    return buf;
}

void netbuf_free(struct netbuf *buf) {
    if (!buf) return;
    buf->next = free_list;
    free_list = buf;
    free_count++;
}

int netbuf_free_count(void) {
    return free_count;
}

KAOS_EXPORT(netbuf_alloc)
KAOS_EXPORT(netbuf_free)
