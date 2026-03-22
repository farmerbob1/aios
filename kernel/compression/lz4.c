/* AIOS — LZ4 Block Compression
 * Minimal freestanding LZ4 block format implementation.
 *
 * LZ4 block format:
 *   Sequence of tokens. Each token:
 *     [token byte] [optional extra literal len] [literal bytes] [offset LE16] [optional extra match len]
 *   Token byte: high nibble = literal length (0-15), low nibble = match length - 4 (0-15)
 *   If nibble == 15, additional bytes of 255 follow until a byte < 255 (add all).
 *   Match offset is 2 bytes little-endian (1..65535).
 *   Minimum match length is 4.
 *   Last sequence has no match (literals only, offset omitted). */

#include "lz4.h"
#include "../../include/string.h"
#include "../../include/kaos/export.h"

#define LZ4_MIN_MATCH    4
#define LZ4_HASH_BITS    12
#define LZ4_HASH_SIZE    (1 << LZ4_HASH_BITS)

static inline uint32_t lz4_read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t lz4_hash(uint32_t val) {
    return (val * 2654435761u) >> (32 - LZ4_HASH_BITS);
}

static inline void lz4_write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t lz4_read16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Write a variable-length integer (value >= 15 already subtracted from nibble).
 * Writes as many 255 bytes as needed, then the remainder. */
static inline uint8_t *lz4_write_vlen(uint8_t *op, int len) {
    while (len >= 255) {
        *op++ = 255;
        len -= 255;
    }
    *op++ = (uint8_t)len;
    return op;
}

int chaos_lz4_compress(const void *src, int src_len, void *dst, int dst_capacity) {
    const uint8_t *ip = (const uint8_t *)src;
    const uint8_t *ip_end = ip + src_len;
    const uint8_t *ip_limit = ip_end - LZ4_MIN_MATCH;  /* last position to start a match */
    uint8_t *op = (uint8_t *)dst;
    uint8_t *op_end = op + dst_capacity;

    /* Hash table: maps hash → position offset from src start */
    uint16_t htable[LZ4_HASH_SIZE];
    memset(htable, 0, sizeof(htable));

    if (src_len < LZ4_MIN_MATCH + 1) {
        /* Too short to compress — emit as literals */
        if (op_end - op < 1 + src_len + (src_len >= 15 ? (src_len - 15) / 255 + 1 : 0))
            return -1;
        int lit_len = src_len;
        uint8_t token = (lit_len >= 15) ? 0xF0 : (uint8_t)(lit_len << 4);
        *op++ = token;
        if (lit_len >= 15) op = lz4_write_vlen(op, lit_len - 15);
        memcpy(op, ip, src_len);
        op += src_len;
        return (int)(op - (uint8_t *)dst);
    }

    const uint8_t *anchor = ip;  /* start of pending literals */
    ip++;  /* first byte can't match */

    while (ip <= ip_limit) {
        /* Find a match */
        uint32_t h = lz4_hash(lz4_read32(ip));
        int ref_off = htable[h];
        const uint8_t *ref = (const uint8_t *)src + ref_off;
        htable[h] = (uint16_t)(ip - (const uint8_t *)src);

        /* Check match validity */
        if (ref < (const uint8_t *)src || ip - ref > 65535 ||
            lz4_read32(ref) != lz4_read32(ip)) {
            ip++;
            continue;
        }

        /* Emit pending literals + this match */
        int lit_len = (int)(ip - anchor);

        /* Extend match forward */
        const uint8_t *mp = ip + LZ4_MIN_MATCH;
        const uint8_t *mr = ref + LZ4_MIN_MATCH;
        while (mp < ip_end && mr < ip && *mp == *mr) {
            mp++;
            mr++;
        }
        int match_len = (int)(mp - ip) - LZ4_MIN_MATCH;

        /* Check output space (conservative) */
        int needed = 1 + (lit_len >= 15 ? (lit_len - 15) / 255 + 1 : 0) +
                     lit_len + 2 + (match_len >= 15 ? (match_len - 15) / 255 + 1 : 0);
        if (op + needed > op_end) return -1;

        /* Token byte */
        uint8_t *token_ptr = op++;
        uint8_t token = 0;

        /* Literal length */
        if (lit_len >= 15) {
            token = 0xF0;
            op = lz4_write_vlen(op, lit_len - 15);
        } else {
            token = (uint8_t)(lit_len << 4);
        }

        /* Copy literals */
        memcpy(op, anchor, lit_len);
        op += lit_len;

        /* Match offset (little-endian) */
        uint16_t offset = (uint16_t)(ip - ref);
        lz4_write16(op, offset);
        op += 2;

        /* Match length */
        if (match_len >= 15) {
            token |= 0x0F;
            op = lz4_write_vlen(op, match_len - 15);
        } else {
            token |= (uint8_t)match_len;
        }

        *token_ptr = token;

        /* Advance past the match */
        ip = mp;
        anchor = ip;

        /* Update hash for skipped positions */
        if (ip <= ip_limit) {
            htable[lz4_hash(lz4_read32(ip - 2))] = (uint16_t)(ip - 2 - (const uint8_t *)src);
        }
    }

    /* Emit final literals (everything from anchor to end) */
    int last_lit = (int)(ip_end - anchor);
    if (last_lit > 0) {
        int needed = 1 + (last_lit >= 15 ? (last_lit - 15) / 255 + 1 : 0) + last_lit;
        if (op + needed > op_end) return -1;

        uint8_t token = (last_lit >= 15) ? 0xF0 : (uint8_t)(last_lit << 4);
        *op++ = token;
        if (last_lit >= 15) op = lz4_write_vlen(op, last_lit - 15);
        memcpy(op, anchor, last_lit);
        op += last_lit;
    }

    return (int)(op - (uint8_t *)dst);
}

int chaos_lz4_decompress(const void *src, int src_len, void *dst, int dst_capacity) {
    const uint8_t *ip = (const uint8_t *)src;
    const uint8_t *ip_end = ip + src_len;
    uint8_t *op = (uint8_t *)dst;
    uint8_t *op_end = op + dst_capacity;

    while (ip < ip_end) {
        /* Read token */
        uint8_t token = *ip++;

        /* Literal length */
        int lit_len = (token >> 4) & 0x0F;
        if (lit_len == 15) {
            int s;
            do {
                if (ip >= ip_end) return -1;
                s = *ip++;
                lit_len += s;
            } while (s == 255);
        }

        /* Copy literals */
        if (ip + lit_len > ip_end) return -1;
        if (op + lit_len > op_end) return -1;
        memcpy(op, ip, lit_len);
        ip += lit_len;
        op += lit_len;

        /* Check if this is the last sequence (no match after final literals) */
        if (ip >= ip_end) break;

        /* Read match offset */
        if (ip + 2 > ip_end) return -1;
        uint16_t offset = lz4_read16(ip);
        ip += 2;
        if (offset == 0) return -2;  /* invalid offset */

        uint8_t *match = op - offset;
        if (match < (uint8_t *)dst) return -2;  /* offset before start */

        /* Match length */
        int match_len = (token & 0x0F) + LZ4_MIN_MATCH;
        if ((token & 0x0F) == 15) {
            int s;
            do {
                if (ip >= ip_end) return -1;
                s = *ip++;
                match_len += s;
            } while (s == 255);
        }

        /* Copy match (byte-by-byte for overlapping copies) */
        if (op + match_len > op_end) return -1;
        for (int i = 0; i < match_len; i++) {
            op[i] = match[i];
        }
        op += match_len;
    }

    return (int)(op - (uint8_t *)dst);
}

KAOS_EXPORT(chaos_lz4_compress)
KAOS_EXPORT(chaos_lz4_decompress)
