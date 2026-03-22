/* AIOS — LZ4 Block Compression
 * Minimal freestanding implementation of LZ4 block format.
 * No dependencies beyond types.h. */

#pragma once

#include "../../include/types.h"

/* Compress src_len bytes from src into dst.
 * Returns compressed size on success, negative on error.
 * dst_capacity must be >= src_len (worst case: incompressible data grows slightly).
 * Recommended: dst_capacity = src_len + src_len/255 + 16 */
int chaos_lz4_compress(const void *src, int src_len, void *dst, int dst_capacity);

/* Decompress src_len bytes from src into dst.
 * dst_capacity is the maximum output size (must match original uncompressed size).
 * Returns decompressed size on success, negative on error. */
int chaos_lz4_decompress(const void *src, int src_len, void *dst, int dst_capacity);
