/* AIOS v2 — BearSSL TLS Port (Phase 11e)
 *
 * Provides a simple TLS connection API built on BearSSL + lwIP TCP.
 * BearSSL's engine model: we feed ciphertext in, get plaintext out (and vice versa).
 * The engine tells us what it needs (sendrec/recvrec/sendapp/recvapp).
 *
 * I/O flow:
 *   App plaintext → BearSSL encrypt → lwIP TCP send (ciphertext)
 *   lwIP TCP recv (ciphertext) → BearSSL decrypt → App plaintext */

#include "bearssl_port.h"
#include "entropy.h"
#include "../../include/types.h"
#include "../../include/string.h"
#include "../heap.h"
#include "../../drivers/serial.h"

#include "bearssl.h"
#include "lwip/tcp.h"

/* Kernel API */
extern void task_sleep(uint32_t ms);
extern uint32_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);
extern void lwip_poll(void);

/* ── Trust Anchors (minimal set for HTTPS) ──────────────── */
/* Defined in trust_anchors.c */
extern const br_x509_trust_anchor *AIOS_TAs;
extern const size_t AIOS_TAs_NUM;

/* ── PRNG State ─────────────────────────────────────────── */
static br_hmac_drbg_context rng_ctx;
static bool rng_initialized = false;

void bearssl_init(void) {
    /* Seed PRNG from entropy pool */
    uint8_t seed[48];  /* 384 bits */
    if (entropy_get(seed, sizeof(seed)) < 0) {
        serial_print("[bearssl] WARNING: entropy not ready, using weak seed\n");
        /* Fallback: use RDTSC directly (weak but functional) */
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        memset(seed, 0, sizeof(seed));
        memcpy(seed, &lo, 4);
        memcpy(seed + 4, &hi, 4);
    }

    br_hmac_drbg_init(&rng_ctx, &br_sha256_vtable, seed, sizeof(seed));
    rng_initialized = true;
    serial_print("[bearssl] HMAC_DRBG PRNG initialized\n");
}

/* ── TLS Connection Structure ───────────────────────────── */

#define TLS_IOBUF_SIZE  (16384 + 325)  /* BR_SSL_BUFSIZE_BIDI */

struct tls_conn {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    uint8_t iobuf[TLS_IOBUF_SIZE];

    /* lwIP TCP state */
    struct tcp_pcb *pcb;
    uint8_t  recv_buf[65536];   /* 64KB TLS recv buffer */
    uint32_t recv_len;
    bool     recv_eof;
    bool     tcp_err;
    bool     active;
};

#define MAX_TLS_CONNS 4
static struct tls_conn *tls_conns[MAX_TLS_CONNS];

static uint32_t millis(void) {
    return timer_get_ticks() * 1000 / timer_get_frequency();
}

/* ── lwIP TCP callbacks for TLS connection ──────────────── */

static err_t tls_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct tls_conn *conn = (struct tls_conn *)arg;
    (void)err;

    if (!p) {
        conn->recv_eof = true;
        return ERR_OK;
    }

    /* Copy what fits into recv_buf, ACK only what we copied */
    uint32_t space = sizeof(conn->recv_buf) - conn->recv_len;
    uint16_t copy = (p->tot_len <= space) ? p->tot_len : (uint16_t)space;
    if (copy > 0) {
        pbuf_copy_partial(p, conn->recv_buf + conn->recv_len, copy, 0);
        conn->recv_len += copy;
        tcp_recved(pcb, copy);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void tls_tcp_err_cb(void *arg, err_t err) {
    struct tls_conn *conn = (struct tls_conn *)arg;
    (void)err;
    conn->tcp_err = true;
    conn->pcb = NULL;
}

/* ── BearSSL Engine I/O Pump ────────────────────────────── */

/* Run one cycle of the BearSSL engine:
 * - If engine wants to send ciphertext, send via TCP
 * - If engine wants to receive ciphertext, feed from our TCP recv buffer
 * Returns: 1 if progress made, 0 if blocked, -1 on error */
static int tls_engine_pump(struct tls_conn *conn) {
    unsigned state = br_ssl_engine_current_state(&conn->sc.eng);
    int progress = 0;

    if (state == BR_SSL_CLOSED) return -1;

    /* Engine wants to send ciphertext → push to TCP */
    if (state & BR_SSL_SENDREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(&conn->sc.eng, &len);
        if (buf && len > 0 && conn->pcb) {
            uint16_t send_len = (len > tcp_sndbuf(conn->pcb)) ?
                                tcp_sndbuf(conn->pcb) : (uint16_t)len;
            if (send_len > 0) {
                err_t err = tcp_write(conn->pcb, buf, send_len, TCP_WRITE_FLAG_COPY);
                if (err == ERR_OK) {
                    br_ssl_engine_sendrec_ack(&conn->sc.eng, send_len);
                    tcp_output(conn->pcb);
                    progress = 1;
                }
            }
        }
    }

    /* Engine wants ciphertext → feed from TCP recv buffer */
    if (state & BR_SSL_RECVREC) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvrec_buf(&conn->sc.eng, &len);
        if (buf && len > 0 && conn->recv_len > 0) {
            uint16_t feed = conn->recv_len;
            if (feed > (uint16_t)len) feed = (uint16_t)len;
            memcpy(buf, conn->recv_buf, feed);
            br_ssl_engine_recvrec_ack(&conn->sc.eng, feed);
            /* Shift remaining data */
            if (feed < conn->recv_len) {
                memmove(conn->recv_buf, conn->recv_buf + feed, conn->recv_len - feed);
            }
            conn->recv_len -= feed;
            progress = 1;
        } else if (conn->recv_eof) {
            br_ssl_engine_close(&conn->sc.eng);
            progress = 1;
        }
    }

    return progress;
}

/* ── Public API ─────────────────────────────────────────── */

struct tls_conn *tls_conn_create(void *tcp_pcb, const char *hostname) {
    if (!rng_initialized) return NULL;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TLS_CONNS; i++) {
        if (!tls_conns[i]) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    struct tls_conn *conn = (struct tls_conn *)kmalloc(sizeof(struct tls_conn));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(*conn));

    conn->pcb = (struct tcp_pcb *)tcp_pcb;
    conn->active = true;

    /* Set up TCP callbacks for TLS */
    tcp_arg(conn->pcb, conn);
    tcp_recv(conn->pcb, tls_tcp_recv_cb);
    tcp_err(conn->pcb, tls_tcp_err_cb);

    /* Initialize SSL client context */
    if (AIOS_TAs_NUM > 0) {
        /* Full initialization with certificate validation */
        /* init_full sets up cipher suites, x509 engine, and trust anchors */
        br_ssl_client_init_full(&conn->sc, &conn->xc,
                                AIOS_TAs, AIOS_TAs_NUM);

        /* Set time AFTER init_full (init_full reinitializes xc).
         * If NTP hasn't synced yet, wait up to 10 seconds for it. */
        {
            extern void sysclock_bearssl_time(uint32_t *d, uint32_t *s);
            extern bool sysclock_synced(void);
            uint32_t bdays, bsecs;

            if (!sysclock_synced()) {
                serial_printf("[bearssl] waiting for NTP sync...\n");
                for (int i = 0; i < 100 && !sysclock_synced(); i++) {
                    lwip_poll();
                    task_sleep(100);
                }
            }

            sysclock_bearssl_time(&bdays, &bsecs);
            br_x509_minimal_set_time(&conn->xc, bdays, bsecs);
        }
    } else {
        /* No trust anchors — init with no-verify for testing.
         * BearSSL requires at least the cipher suites to be set up. */
        br_ssl_client_init_full(&conn->sc, &conn->xc,
                                NULL, 0);
        /* Override X.509 to accept any certificate */
        br_x509_minimal_init(&conn->xc, &br_sha256_vtable, NULL, 0);
        br_ssl_engine_set_x509(&conn->sc.eng, &conn->xc.vtable);
    }

    /* Set I/O buffer (bidirectional) */
    br_ssl_engine_set_buffer(&conn->sc.eng, conn->iobuf,
                             sizeof(conn->iobuf), 1);

    /* Inject entropy */
    uint8_t entropy_buf[32];
    br_hmac_drbg_generate(&rng_ctx, entropy_buf, sizeof(entropy_buf));
    br_ssl_engine_inject_entropy(&conn->sc.eng,
                                 entropy_buf, sizeof(entropy_buf));

    /* Start TLS handshake (sets SNI hostname) */
    br_ssl_client_reset(&conn->sc, hostname, 0);

    tls_conns[slot] = conn;
    return conn;
}

int tls_conn_handshake(struct tls_conn *conn, int timeout_ms) {
    uint32_t start = millis();

    while (1) {
        /* Pump the engine */
        lwip_poll();
        int rc = tls_engine_pump(conn);

        /* Check if handshake is complete */
        unsigned state = br_ssl_engine_current_state(&conn->sc.eng);
        if (state & BR_SSL_SENDAPP) {
            /* Handshake done — we can send application data */
            return 0;
        }
        if (state == BR_SSL_CLOSED || rc < 0 || conn->tcp_err) {
            int err = br_ssl_engine_last_error(&conn->sc.eng);
            serial_printf("[bearssl] handshake failed: err=%d\n", err);
            return -1;
        }

        if ((int)(millis() - start) > timeout_ms) {
            serial_print("[bearssl] handshake timeout\n");
            return -1;
        }

        task_sleep(4);
    }
}

int tls_conn_write(struct tls_conn *conn, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t total = 0;

    while (total < len) {
        unsigned state = br_ssl_engine_current_state(&conn->sc.eng);
        if (state == BR_SSL_CLOSED || conn->tcp_err) return -1;

        if (state & BR_SSL_SENDAPP) {
            size_t blen;
            unsigned char *buf = br_ssl_engine_sendapp_buf(&conn->sc.eng, &blen);
            if (buf && blen > 0) {
                size_t chunk = len - total;
                if (chunk > blen) chunk = blen;
                memcpy(buf, ptr + total, chunk);
                br_ssl_engine_sendapp_ack(&conn->sc.eng, chunk);
                br_ssl_engine_flush(&conn->sc.eng, 0);
                total += chunk;
            }
        }

        tls_engine_pump(conn);
        lwip_poll();
        if (total < len) task_sleep(1);
    }

    return (int)total;
}

int tls_conn_read(struct tls_conn *conn, void *buf, size_t len, int timeout_ms) {
    uint32_t start = millis();
    size_t total = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (total < len) {
        /* Pump multiple times to drain as much data as possible */
        for (int pump = 0; pump < 8; pump++) {
            lwip_poll();
            if (tls_engine_pump(conn) <= 0) break;
        }

        unsigned state = br_ssl_engine_current_state(&conn->sc.eng);

        if (state & BR_SSL_RECVAPP) {
            size_t blen;
            unsigned char *app = br_ssl_engine_recvapp_buf(&conn->sc.eng, &blen);
            if (app && blen > 0) {
                size_t want = len - total;
                size_t copy = (blen > want) ? want : blen;
                memcpy(dst + total, app, copy);
                br_ssl_engine_recvapp_ack(&conn->sc.eng, copy);
                total += copy;
                /* Reset timeout on data received */
                start = millis();
                continue;  /* Try to get more immediately */
            }
        }

        if (total > 0) {
            /* We have some data — return what we have if engine is blocked */
            if (!(state & BR_SSL_RECVAPP) && !(state & BR_SSL_RECVREC)) {
                break;
            }
        }

        if (state == BR_SSL_CLOSED || conn->tcp_err) {
            break;  /* Connection closed — return what we have */
        }

        if ((int)(millis() - start) > timeout_ms) {
            if (total > 0) break;  /* Return partial data */
            return -1;  /* Timeout with no data */
        }

        task_sleep(2);  /* Shorter sleep for faster throughput */
    }

    return (int)total;
}

void tls_conn_close(struct tls_conn *conn) {
    if (!conn) return;

    br_ssl_engine_close(&conn->sc.eng);

    /* Flush remaining TLS close_notify */
    for (int i = 0; i < 10; i++) {
        lwip_poll();
        if (tls_engine_pump(conn) <= 0) break;
        task_sleep(4);
    }

    /* Close TCP */
    if (conn->pcb) {
        tcp_arg(conn->pcb, NULL);
        tcp_recv(conn->pcb, NULL);
        tcp_err(conn->pcb, NULL);
        tcp_close(conn->pcb);
        conn->pcb = NULL;
    }

    /* Remove from slot table */
    for (int i = 0; i < MAX_TLS_CONNS; i++) {
        if (tls_conns[i] == conn) {
            tls_conns[i] = NULL;
            break;
        }
    }

    kfree(conn);
}
