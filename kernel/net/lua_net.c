/* AIOS v2 — Lua Networking Bindings (Phase 11d)
 *
 * Provides aios.net.* API for Lua scripts:
 *   aios.net.ifconfig()                     → {ip, mask, gw, mac}
 *   aios.net.dns_resolve(host, timeout_ms)  → ip_string
 *   aios.net.tcp_connect(host, port, timeout_ms) → sock_id
 *   aios.net.tcp_send(sock_id, data)        → bytes_sent
 *   aios.net.tcp_recv(sock_id, max, timeout_ms) → data
 *   aios.net.tcp_close(sock_id)             → true
 *
 * Uses lwIP raw TCP API with blocking poll loops. */

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "netif_bridge.h"
#include "../../include/types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

/* Lua headers */
#include "lua.h"
#include "lauxlib.h"

/* Kernel API */
extern void task_sleep(uint32_t ms);
extern uint32_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);
extern void lwip_poll(void);

/* lwIP netif (defined in lwip_netif.c) */
extern struct netif aios_netif;

/* ── Socket State ───────────────────────────────────────── */

#define MAX_SOCKETS    16
#define RECV_BUF_SIZE  8192

struct lua_socket {
    struct tcp_pcb *pcb;
    uint8_t  recv_buf[RECV_BUF_SIZE];
    uint16_t recv_len;
    uint16_t recv_read;
    bool     active;
    bool     connected;
    bool     connect_err;
    bool     closed_remote;
    err_t    last_err;
};

static struct lua_socket sockets[MAX_SOCKETS];

static int alloc_socket(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            memset(&sockets[i], 0, sizeof(struct lua_socket));
            sockets[i].active = true;
            return i;
        }
    }
    return -1;
}

static void free_socket(int idx) {
    if (idx >= 0 && idx < MAX_SOCKETS) {
        sockets[idx].active = false;
        sockets[idx].pcb = NULL;
    }
}

static uint32_t millis(void) {
    return timer_get_ticks() * 1000 / timer_get_frequency();
}

/* ── lwIP TCP Callbacks ─────────────────────────────────── */

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    struct lua_socket *s = (struct lua_socket *)arg;
    if (err == ERR_OK) {
        s->connected = true;
    } else {
        s->connect_err = true;
        s->last_err = err;
    }
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct lua_socket *s = (struct lua_socket *)arg;
    (void)err;

    if (p == NULL) {
        /* Remote closed connection */
        s->closed_remote = true;
        return ERR_OK;
    }

    /* Copy data into socket recv buffer */
    uint16_t space = RECV_BUF_SIZE - s->recv_len;
    uint16_t copy_len = p->tot_len;
    if (copy_len > space) copy_len = space;

    if (copy_len > 0) {
        pbuf_copy_partial(p, s->recv_buf + s->recv_len, copy_len, 0);
        s->recv_len += copy_len;
        tcp_recved(pcb, copy_len);
    }

    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    struct lua_socket *s = (struct lua_socket *)arg;
    s->last_err = err;
    s->connect_err = true;
    s->closed_remote = true;
    s->pcb = NULL;  /* PCB is freed by lwIP on error */
}

/* ── DNS Callback State ─────────────────────────────────── */

static ip_addr_t dns_result_addr;
static volatile bool dns_done = false;
static volatile bool dns_ok = false;

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name; (void)arg;
    if (addr) {
        dns_result_addr = *addr;
        dns_ok = true;
    }
    dns_done = true;
}

/* ── Lua Functions ──────────────────────────────────────── */

/* aios.net.ifconfig() → table {ip, mask, gw, mac} */
static int l_net_ifconfig(lua_State *L) {
    lua_newtable(L);

    uint32_t ip = aios_netif.ip_addr.addr;
    uint32_t mask = aios_netif.netmask.addr;
    uint32_t gw = aios_netif.gw.addr;

    char buf[20];

    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "ip");

    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             mask & 0xFF, (mask >> 8) & 0xFF, (mask >> 16) & 0xFF, (mask >> 24) & 0xFF);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "mask");

    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             gw & 0xFF, (gw >> 8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "gw");

    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             aios_netif.hwaddr[0], aios_netif.hwaddr[1], aios_netif.hwaddr[2],
             aios_netif.hwaddr[3], aios_netif.hwaddr[4], aios_netif.hwaddr[5]);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "mac");

    return 1;
}

/* aios.net.dns_resolve(hostname, timeout_ms) → ip_string or nil */
static int l_net_dns_resolve(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int timeout = (int)luaL_optinteger(L, 2, 5000);

    /* Try parsing as IP address first */
    ip_addr_t addr;
    if (ip4addr_aton(host, ip_2_ip4(&addr))) {
        char buf[20];
        uint32_t ip = addr.addr;
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        lua_pushstring(L, buf);
        return 1;
    }

    /* DNS lookup */
    dns_done = false;
    dns_ok = false;
    err_t err = dns_gethostbyname(host, &dns_result_addr, dns_found_cb, NULL);

    if (err == ERR_OK) {
        /* Cached result */
        dns_ok = true;
        dns_done = true;
    } else if (err != ERR_INPROGRESS) {
        lua_pushnil(L);
        lua_pushstring(L, "dns lookup failed");
        return 2;
    }

    /* Poll until resolved or timeout */
    uint32_t start = millis();
    while (!dns_done) {
        if ((int)(millis() - start) > timeout) {
            lua_pushnil(L);
            lua_pushstring(L, "dns timeout");
            return 2;
        }
        lwip_poll();
        task_sleep(4);
    }

    if (!dns_ok) {
        lua_pushnil(L);
        lua_pushstring(L, "dns not found");
        return 2;
    }

    char buf[20];
    uint32_t ip = dns_result_addr.addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    lua_pushstring(L, buf);
    return 1;
}

/* aios.net.tcp_connect(host, port, timeout_ms) → sock_id or nil, err */
static int l_net_tcp_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int timeout = (int)luaL_optinteger(L, 3, 5000);

    /* Resolve hostname */
    ip_addr_t addr;
    if (!ip4addr_aton(host, ip_2_ip4(&addr))) {
        /* Need DNS resolution */
        dns_done = false;
        dns_ok = false;
        err_t err = dns_gethostbyname(host, &addr, dns_found_cb, NULL);
        if (err == ERR_OK) {
            dns_ok = true;
            dns_done = true;
        } else if (err == ERR_INPROGRESS) {
            uint32_t start = millis();
            while (!dns_done) {
                if ((int)(millis() - start) > timeout) {
                    lua_pushnil(L);
                    lua_pushstring(L, "dns timeout");
                    return 2;
                }
                lwip_poll();
                task_sleep(4);
            }
            if (!dns_ok) {
                lua_pushnil(L);
                lua_pushstring(L, "dns failed");
                return 2;
            }
            addr = dns_result_addr;
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "dns error");
            return 2;
        }
    }

    /* Allocate socket */
    int sid = alloc_socket();
    if (sid < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free sockets");
        return 2;
    }
    struct lua_socket *s = &sockets[sid];

    /* Create TCP PCB */
    s->pcb = tcp_new();
    if (!s->pcb) {
        free_socket(sid);
        lua_pushnil(L);
        lua_pushstring(L, "tcp_new failed");
        return 2;
    }

    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, tcp_recv_cb);
    tcp_err(s->pcb, tcp_err_cb);

    /* Start async connect */
    err_t err = tcp_connect(s->pcb, &addr, (u16_t)port, tcp_connected_cb);
    if (err != ERR_OK) {
        tcp_close(s->pcb);
        free_socket(sid);
        lua_pushnil(L);
        lua_pushstring(L, "connect failed");
        return 2;
    }

    /* Poll until connected or timeout */
    uint32_t start = millis();
    while (!s->connected && !s->connect_err) {
        if ((int)(millis() - start) > timeout) {
            tcp_abort(s->pcb);
            s->pcb = NULL;
            free_socket(sid);
            lua_pushnil(L);
            lua_pushstring(L, "connect timeout");
            return 2;
        }
        lwip_poll();
        task_sleep(4);
    }

    if (s->connect_err) {
        free_socket(sid);
        lua_pushnil(L);
        lua_pushstring(L, "connect error");
        return 2;
    }

    lua_pushinteger(L, sid);
    return 1;
}

/* aios.net.tcp_send(sock_id, data) → bytes_sent or nil, err */
static int l_net_tcp_send(lua_State *L) {
    int sid = (int)luaL_checkinteger(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    if (sid < 0 || sid >= MAX_SOCKETS || !sockets[sid].active) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid socket");
        return 2;
    }

    struct lua_socket *s = &sockets[sid];
    if (!s->pcb || s->closed_remote) {
        lua_pushnil(L);
        lua_pushstring(L, "socket closed");
        return 2;
    }

    /* Write data (lwIP buffers it internally) */
    err_t err = tcp_write(s->pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "write error");
        return 2;
    }
    tcp_output(s->pcb);

    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

/* aios.net.tcp_recv(sock_id, max_len, timeout_ms) → data or nil */
static int l_net_tcp_recv(lua_State *L) {
    int sid = (int)luaL_checkinteger(L, 1);
    int max_len = (int)luaL_optinteger(L, 2, 4096);
    int timeout = (int)luaL_optinteger(L, 3, 5000);

    if (sid < 0 || sid >= MAX_SOCKETS || !sockets[sid].active) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid socket");
        return 2;
    }

    struct lua_socket *s = &sockets[sid];

    /* Poll until data available or timeout */
    uint32_t start = millis();
    while (s->recv_len == 0 && !s->closed_remote) {
        if ((int)(millis() - start) > timeout) {
            lua_pushnil(L);
            lua_pushstring(L, "recv timeout");
            return 2;
        }
        lwip_poll();
        task_sleep(4);
    }

    if (s->recv_len == 0) {
        /* Connection closed, no more data */
        lua_pushnil(L);
        return 1;
    }

    /* Return available data up to max_len */
    uint16_t ret_len = s->recv_len;
    if (ret_len > (uint16_t)max_len) ret_len = (uint16_t)max_len;

    lua_pushlstring(L, (const char *)s->recv_buf, ret_len);

    /* Shift remaining data */
    if (ret_len < s->recv_len) {
        memmove(s->recv_buf, s->recv_buf + ret_len, s->recv_len - ret_len);
    }
    s->recv_len -= ret_len;

    return 1;
}

/* aios.net.tcp_close(sock_id) → true */
static int l_net_tcp_close(lua_State *L) {
    int sid = (int)luaL_checkinteger(L, 1);

    if (sid < 0 || sid >= MAX_SOCKETS || !sockets[sid].active) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct lua_socket *s = &sockets[sid];
    if (s->pcb) {
        /* Flush: let lwIP transmit any queued data before closing */
        tcp_output(s->pcb);
        for (int i = 0; i < 10; i++) {
            lwip_poll();
            if (s->pcb && s->pcb->unsent == NULL && s->pcb->unacked == NULL)
                break;
            task_sleep(4);
        }
        if (s->pcb) {
            tcp_arg(s->pcb, NULL);
            tcp_recv(s->pcb, NULL);
            tcp_err(s->pcb, NULL);
            tcp_close(s->pcb);
            s->pcb = NULL;
        }
    }
    free_socket(sid);

    lua_pushboolean(L, 1);
    return 1;
}

/* ── Server (Listen/Accept) ─────────────────────────────── */

#define MAX_SERVERS       4
#define ACCEPT_QUEUE_SIZE 8

struct lua_server {
    struct tcp_pcb *listen_pcb;
    int accept_queue[ACCEPT_QUEUE_SIZE];  /* socket IDs, not raw PCBs */
    int accept_head;
    int accept_tail;
    int accept_count;
    bool active;
};

static struct lua_server servers[MAX_SERVERS];

static err_t server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    struct lua_server *sv = (struct lua_server *)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;

    if (sv->accept_count >= ACCEPT_QUEUE_SIZE) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* Allocate socket and set callbacks NOW so incoming data isn't dropped */
    int sock = alloc_socket();
    if (sock < 0) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    struct lua_socket *s = &sockets[sock];
    s->pcb = newpcb;
    s->connected = true;
    tcp_arg(newpcb, s);
    tcp_recv(newpcb, tcp_recv_cb);
    tcp_err(newpcb, tcp_err_cb);

    sv->accept_queue[sv->accept_tail] = sock;
    sv->accept_tail = (sv->accept_tail + 1) % ACCEPT_QUEUE_SIZE;
    sv->accept_count++;
    return ERR_OK;
}

/* aios.net.tcp_listen(port) → server_id or nil, err */
static int l_net_tcp_listen(lua_State *L) {
    int port = (int)luaL_checkinteger(L, 1);

    /* Find free server slot */
    int sid = -1;
    for (int i = 0; i < MAX_SERVERS; i++) {
        if (!servers[i].active) { sid = i; break; }
    }
    if (sid < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free server slots");
        return 2;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        lua_pushnil(L);
        lua_pushstring(L, "tcp_new failed");
        return 2;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, (u16_t)port);
    if (err != ERR_OK) {
        tcp_close(pcb);
        lua_pushnil(L);
        lua_pushstring(L, "bind failed");
        return 2;
    }

    struct tcp_pcb *listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) {
        tcp_close(pcb);
        lua_pushnil(L);
        lua_pushstring(L, "listen failed");
        return 2;
    }

    memset(&servers[sid], 0, sizeof(struct lua_server));
    servers[sid].listen_pcb = listen_pcb;
    servers[sid].active = true;

    tcp_arg(listen_pcb, &servers[sid]);
    tcp_accept(listen_pcb, server_accept_cb);

    serial_printf("[net] TCP server listening on port %d (server %d)\n", port, sid);

    /* Return server ID offset by 2000 */
    lua_pushinteger(L, 2000 + sid);
    return 1;
}

/* aios.net.tcp_accept(server_id, timeout_ms) → sock_id or nil */
static int l_net_tcp_accept(lua_State *L) {
    int sid = (int)luaL_checkinteger(L, 1) - 2000;
    int timeout = (int)luaL_optinteger(L, 2, 5000);

    if (sid < 0 || sid >= MAX_SERVERS || !servers[sid].active) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid server");
        return 2;
    }

    struct lua_server *sv = &servers[sid];

    /* Poll until a connection arrives or timeout */
    uint32_t start = millis();
    while (sv->accept_count == 0) {
        if ((int)(millis() - start) > timeout) {
            lua_pushnil(L);
            return 1;  /* nil = timeout, not an error */
        }
        lwip_poll();
        task_sleep(4);
    }

    /* Dequeue the pre-allocated socket ID */
    int sock = sv->accept_queue[sv->accept_head];
    sv->accept_head = (sv->accept_head + 1) % ACCEPT_QUEUE_SIZE;
    sv->accept_count--;

    lua_pushinteger(L, sock);
    return 1;
}

/* aios.net.tcp_server_close(server_id) → true */
static int l_net_tcp_server_close(lua_State *L) {
    int sid = (int)luaL_checkinteger(L, 1) - 2000;

    if (sid >= 0 && sid < MAX_SERVERS && servers[sid].active) {
        if (servers[sid].listen_pcb) {
            tcp_close(servers[sid].listen_pcb);
            servers[sid].listen_pcb = NULL;
        }
        /* Close any queued sockets */
        while (servers[sid].accept_count > 0) {
            int sock = servers[sid].accept_queue[servers[sid].accept_head];
            servers[sid].accept_head = (servers[sid].accept_head + 1) % ACCEPT_QUEUE_SIZE;
            servers[sid].accept_count--;
            if (sock >= 0 && sock < MAX_SOCKETS && sockets[sock].active) {
                if (sockets[sock].pcb) {
                    tcp_abort(sockets[sock].pcb);
                    sockets[sock].pcb = NULL;
                }
                free_socket(sock);
            }
        }
        servers[sid].active = false;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── TLS Functions ──────────────────────────────────────── */

#include "bearssl_port.h"

/* TLS socket state — separate from plain TCP sockets */
/* Temporary TCP connect state for TLS */
static volatile bool tls_tcp_connected = false;
static volatile bool tls_tcp_err_flag = false;

static err_t tls_connect_cb(void *arg, struct tcp_pcb *p, err_t e) {
    (void)arg; (void)p;
    if (e == ERR_OK) tls_tcp_connected = true;
    else tls_tcp_err_flag = true;
    return ERR_OK;
}

#define MAX_TLS_SOCKETS 4

static struct {
    struct tls_conn *conn;
    bool active;
} tls_sockets[MAX_TLS_SOCKETS];

/* aios.net.tls_connect(host, port, timeout_ms) → tls_id or nil, err */
static int l_net_tls_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int timeout = (int)luaL_optinteger(L, 3, 10000);

    /* Resolve hostname first */
    ip_addr_t addr;
    if (!ip4addr_aton(host, ip_2_ip4(&addr))) {
        dns_done = false;
        dns_ok = false;
        err_t err = dns_gethostbyname(host, &addr, dns_found_cb, NULL);
        if (err == ERR_OK) {
            dns_ok = true; dns_done = true;
        } else if (err == ERR_INPROGRESS) {
            uint32_t start = millis();
            while (!dns_done) {
                if ((int)(millis() - start) > timeout) {
                    lua_pushnil(L); lua_pushstring(L, "dns timeout"); return 2;
                }
                lwip_poll(); task_sleep(4);
            }
            if (!dns_ok) { lua_pushnil(L); lua_pushstring(L, "dns failed"); return 2; }
            addr = dns_result_addr;
        } else {
            lua_pushnil(L); lua_pushstring(L, "dns error"); return 2;
        }
    }

    /* Create TCP connection first */
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { lua_pushnil(L); lua_pushstring(L, "tcp_new failed"); return 2; }

    tls_tcp_connected = false;
    tls_tcp_err_flag = false;
    tcp_arg(pcb, NULL);
    tcp_err(pcb, NULL);

    err_t err = tcp_connect(pcb, &addr, (u16_t)port, tls_connect_cb);
    if (err != ERR_OK) {
        tcp_close(pcb);
        lua_pushnil(L); lua_pushstring(L, "connect failed"); return 2;
    }

    /* Wait for TCP connection */
    uint32_t start = millis();
    while (!tls_tcp_connected && !tls_tcp_err_flag) {
        if ((int)(millis() - start) > timeout) {
            tcp_abort(pcb);
            lua_pushnil(L); lua_pushstring(L, "connect timeout"); return 2;
        }
        lwip_poll(); task_sleep(4);
    }
    if (tls_tcp_err_flag) {
        lua_pushnil(L); lua_pushstring(L, "connect error"); return 2;
    }

    /* Find free TLS socket slot */
    int tid = -1;
    for (int i = 0; i < MAX_TLS_SOCKETS; i++) {
        if (!tls_sockets[i].active) { tid = i; break; }
    }
    if (tid < 0) {
        tcp_close(pcb);
        lua_pushnil(L); lua_pushstring(L, "no free TLS sockets"); return 2;
    }

    /* Create TLS connection on top of the TCP pcb */
    struct tls_conn *conn = tls_conn_create(pcb, host);
    if (!conn) {
        tcp_close(pcb);
        lua_pushnil(L); lua_pushstring(L, "TLS init failed"); return 2;
    }

    /* Perform TLS handshake */
    int remaining = timeout - (int)(millis() - start);
    if (remaining < 1000) remaining = 1000;
    if (tls_conn_handshake(conn, remaining) < 0) {
        tls_conn_close(conn);
        lua_pushnil(L); lua_pushstring(L, "TLS handshake failed"); return 2;
    }

    tls_sockets[tid].conn = conn;
    tls_sockets[tid].active = true;

    /* Return TLS socket ID (offset by 1000 to distinguish from TCP sockets) */
    lua_pushinteger(L, 1000 + tid);
    return 1;
}

/* aios.net.tls_send(tls_id, data) → bytes or nil, err */
static int l_net_tls_send(lua_State *L) {
    int tid = (int)luaL_checkinteger(L, 1) - 1000;
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    if (tid < 0 || tid >= MAX_TLS_SOCKETS || !tls_sockets[tid].active) {
        lua_pushnil(L); lua_pushstring(L, "invalid TLS socket"); return 2;
    }

    int n = tls_conn_write(tls_sockets[tid].conn, data, len);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, "TLS write error"); return 2; }

    lua_pushinteger(L, n);
    return 1;
}

/* aios.net.tls_recv(tls_id, max_len, timeout_ms) → data or nil */
static int l_net_tls_recv(lua_State *L) {
    int tid = (int)luaL_checkinteger(L, 1) - 1000;
    int max_len = (int)luaL_optinteger(L, 2, 4096);
    int timeout = (int)luaL_optinteger(L, 3, 5000);

    if (tid < 0 || tid >= MAX_TLS_SOCKETS || !tls_sockets[tid].active) {
        lua_pushnil(L); lua_pushstring(L, "invalid TLS socket"); return 2;
    }

    if (max_len > 8192) max_len = 8192;
    uint8_t *buf = kmalloc((size_t)max_len);
    if (!buf) { lua_pushnil(L); lua_pushstring(L, "out of memory"); return 2; }

    int n = tls_conn_read(tls_sockets[tid].conn, buf, (size_t)max_len, timeout);
    if (n > 0) {
        lua_pushlstring(L, (const char *)buf, (size_t)n);
    } else {
        lua_pushnil(L);
    }
    kfree(buf);
    return 1;
}

/* aios.net.tls_close(tls_id) → true */
static int l_net_tls_close(lua_State *L) {
    int tid = (int)luaL_checkinteger(L, 1) - 1000;

    if (tid >= 0 && tid < MAX_TLS_SOCKETS && tls_sockets[tid].active) {
        tls_conn_close(tls_sockets[tid].conn);
        tls_sockets[tid].conn = NULL;
        tls_sockets[tid].active = false;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── UDP Functions ──────────────────────────────────────── */

#include "lwip/udp.h"

#define MAX_UDP_SOCKETS  8
#define UDP_RECV_BUF     4096

struct lua_udp {
    struct udp_pcb *pcb;
    uint8_t  recv_buf[UDP_RECV_BUF];
    uint16_t recv_len;
    ip_addr_t recv_addr;
    uint16_t recv_port;
    bool     active;
    bool     has_data;
};

static struct lua_udp udp_sockets[MAX_UDP_SOCKETS];

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    struct lua_udp *u = (struct lua_udp *)arg;
    if (!p || !u) { if (p) pbuf_free(p); return; }

    uint16_t copy = p->tot_len;
    if (copy > UDP_RECV_BUF) copy = UDP_RECV_BUF;
    pbuf_copy_partial(p, u->recv_buf, copy, 0);
    u->recv_len = copy;
    u->recv_addr = *addr;
    u->recv_port = port;
    u->has_data = true;
    pbuf_free(p);
}

/* aios.net.udp_new(bind_port?) → udp_id or nil, err */
static int l_net_udp_new(lua_State *L) {
    int bind_port = (int)luaL_optinteger(L, 1, 0);

    int uid = -1;
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (!udp_sockets[i].active) { uid = i; break; }
    }
    if (uid < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free UDP sockets");
        return 2;
    }

    struct udp_pcb *pcb = udp_new();
    if (!pcb) {
        lua_pushnil(L);
        lua_pushstring(L, "udp_new failed");
        return 2;
    }

    if (bind_port > 0) {
        err_t err = udp_bind(pcb, IP_ADDR_ANY, (u16_t)bind_port);
        if (err != ERR_OK) {
            udp_remove(pcb);
            lua_pushnil(L);
            lua_pushstring(L, "udp bind failed");
            return 2;
        }
    }

    memset(&udp_sockets[uid], 0, sizeof(struct lua_udp));
    udp_sockets[uid].pcb = pcb;
    udp_sockets[uid].active = true;

    udp_recv(pcb, udp_recv_cb, &udp_sockets[uid]);

    /* UDP IDs offset by 3000 */
    lua_pushinteger(L, 3000 + uid);
    return 1;
}

/* aios.net.udp_sendto(udp_id, host, port, data) → bytes or nil, err */
static int l_net_udp_sendto(lua_State *L) {
    int uid = (int)luaL_checkinteger(L, 1) - 3000;
    const char *host = luaL_checkstring(L, 2);
    int port = (int)luaL_checkinteger(L, 3);
    size_t len;
    const char *data = luaL_checklstring(L, 4, &len);

    if (uid < 0 || uid >= MAX_UDP_SOCKETS || !udp_sockets[uid].active) {
        lua_pushnil(L); lua_pushstring(L, "invalid UDP socket"); return 2;
    }

    /* Resolve host */
    ip_addr_t addr;
    if (!ip4addr_aton(host, ip_2_ip4(&addr))) {
        dns_done = false;
        dns_ok = false;
        err_t err = dns_gethostbyname(host, &addr, dns_found_cb, NULL);
        if (err == ERR_OK) {
            dns_ok = true; dns_done = true;
        } else if (err == ERR_INPROGRESS) {
            uint32_t start = millis();
            while (!dns_done) {
                if ((int)(millis() - start) > 5000) {
                    lua_pushnil(L); lua_pushstring(L, "dns timeout"); return 2;
                }
                lwip_poll(); task_sleep(4);
            }
            if (!dns_ok) { lua_pushnil(L); lua_pushstring(L, "dns failed"); return 2; }
            addr = dns_result_addr;
        } else {
            lua_pushnil(L); lua_pushstring(L, "dns error"); return 2;
        }
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) {
        lua_pushnil(L); lua_pushstring(L, "out of memory"); return 2;
    }
    memcpy(p->payload, data, len);

    err_t err = udp_sendto(udp_sockets[uid].pcb, p, &addr, (u16_t)port);
    pbuf_free(p);

    if (err != ERR_OK) {
        lua_pushnil(L); lua_pushstring(L, "sendto failed"); return 2;
    }

    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

/* aios.net.udp_recvfrom(udp_id, timeout_ms?) → data, ip, port or nil */
static int l_net_udp_recvfrom(lua_State *L) {
    int uid = (int)luaL_checkinteger(L, 1) - 3000;
    int timeout = (int)luaL_optinteger(L, 2, 5000);

    if (uid < 0 || uid >= MAX_UDP_SOCKETS || !udp_sockets[uid].active) {
        lua_pushnil(L); lua_pushstring(L, "invalid UDP socket"); return 2;
    }

    struct lua_udp *u = &udp_sockets[uid];

    uint32_t start = millis();
    while (!u->has_data) {
        if ((int)(millis() - start) > timeout) {
            lua_pushnil(L);
            return 1;
        }
        lwip_poll();
        task_sleep(4);
    }

    lua_pushlstring(L, (const char *)u->recv_buf, u->recv_len);

    /* Return sender IP */
    uint32_t ip = u->recv_addr.addr;
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    lua_pushstring(L, buf);
    lua_pushinteger(L, u->recv_port);

    u->has_data = false;
    u->recv_len = 0;

    return 3;
}

/* aios.net.udp_close(udp_id) → true */
static int l_net_udp_close(lua_State *L) {
    int uid = (int)luaL_checkinteger(L, 1) - 3000;

    if (uid >= 0 && uid < MAX_UDP_SOCKETS && udp_sockets[uid].active) {
        if (udp_sockets[uid].pcb) {
            udp_remove(udp_sockets[uid].pcb);
            udp_sockets[uid].pcb = NULL;
        }
        udp_sockets[uid].active = false;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── Registration ───────────────────────────────────────── */

static const luaL_Reg net_funcs[] = {
    {"ifconfig",    l_net_ifconfig},
    {"dns_resolve", l_net_dns_resolve},
    {"tcp_connect", l_net_tcp_connect},
    {"tcp_send",    l_net_tcp_send},
    {"tcp_recv",    l_net_tcp_recv},
    {"tcp_close",   l_net_tcp_close},
    {"tls_connect", l_net_tls_connect},
    {"tls_send",    l_net_tls_send},
    {"tls_recv",    l_net_tls_recv},
    {"tls_close",       l_net_tls_close},
    {"tcp_listen",      l_net_tcp_listen},
    {"tcp_accept",      l_net_tcp_accept},
    {"tcp_server_close", l_net_tcp_server_close},
    {"udp_new",         l_net_udp_new},
    {"udp_sendto",      l_net_udp_sendto},
    {"udp_recvfrom",    l_net_udp_recvfrom},
    {"udp_close",       l_net_udp_close},
    {NULL, NULL}
};

void aios_register_net(lua_State *L) {
    lua_getglobal(L, "aios");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "aios");
    }
    lua_newtable(L);
    luaL_setfuncs(L, net_funcs, 0);
    lua_setfield(L, -2, "net");
    lua_pop(L, 1);
}
