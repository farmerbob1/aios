/* AIOS v2 — Embedded Root CA Trust Anchors (Phase 11e)
 *
 * Placeholder — no trust anchors embedded yet.
 * TLS will work but certificate validation is not enforced.
 *
 * To add real CAs, use BearSSL's brssl tool:
 *   brssl ta /path/to/roots.pem > trust_anchors.c */

#include "bearssl.h"

/* Empty trust anchor list — TLS handshakes will proceed
 * but server certificates won't be validated against known CAs. */
const size_t AIOS_TAs_NUM = 0;

/* Need a non-NULL array for the API even if empty */
static const br_x509_trust_anchor dummy_ta = { 0 };
const br_x509_trust_anchor *AIOS_TAs = &dummy_ta;
