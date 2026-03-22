/* AIOS v2 — BearSSL TLS Port (Phase 11e) */

#pragma once

#include "../../include/types.h"

/* Initialize BearSSL subsystem (seed PRNG) */
void bearssl_init(void);

/* Opaque TLS connection handle */
struct tls_conn;

/* Create a TLS connection wrapping an existing lwIP TCP pcb.
 * hostname is used for SNI. Returns NULL on failure. */
struct tls_conn *tls_conn_create(void *tcp_pcb, const char *hostname);

/* Perform TLS handshake. Returns 0 on success, -1 on failure.
 * Blocks (via lwip_poll + task_sleep) until handshake completes. */
int tls_conn_handshake(struct tls_conn *conn, int timeout_ms);

/* Send plaintext data over TLS. Returns bytes written or -1. */
int tls_conn_write(struct tls_conn *conn, const void *data, size_t len);

/* Receive plaintext data over TLS. Returns bytes read, 0 for closed, -1 for error.
 * Blocks until data available or timeout. */
int tls_conn_read(struct tls_conn *conn, void *buf, size_t len, int timeout_ms);

/* Close TLS connection and free resources. */
void tls_conn_close(struct tls_conn *conn);
