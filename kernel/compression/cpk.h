/* AIOS — CPK (Chaos Package) Archive Format
 * Simple archive container with optional per-file LZ4 compression.
 *
 * File layout: [header 32B] [file data...] [TOC at toc_offset]
 * TOC is an array of cpk_entry structs. */

#pragma once

#ifdef __AIOS_KERNEL__
#include "../../include/types.h"
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#define CPK_MAGIC    0x43504B47  /* 'CPKG' */
#define CPK_VERSION  1
#define CPK_FLAG_LZ4 (1 << 0)   /* files in this archive are LZ4 compressed */

#define CPK_MAX_HANDLES 8

struct cpk_header {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t toc_offset;        /* byte offset from file start to TOC */
    uint32_t flags;
    uint8_t  reserved[12];
} __attribute__((packed));      /* 32 bytes */

struct cpk_entry {
    char     path[128];         /* relative path within package */
    uint32_t offset;            /* byte offset from file start to this entry's data */
    uint32_t size_compressed;   /* size on disk (== size_original if uncompressed) */
    uint32_t size_original;     /* original uncompressed size */
    uint32_t checksum;          /* CRC-32 of uncompressed data */
} __attribute__((packed));      /* 144 bytes */

#ifdef __AIOS_KERNEL__

/* Open a CPK archive from ChaosFS. Returns handle (0..7) or -1 on error. */
int cpk_open(const char *path);

/* Get number of files in the archive. */
int cpk_file_count(int handle);

/* Get entry info by index. Returns 0 on success, -1 on error. */
int cpk_get_entry(int handle, int index, struct cpk_entry *out);

/* Find entry by path. Returns index (0..n-1) or -1 if not found. */
int cpk_find(int handle, const char *path);

/* Extract a file by index into buf. Decompresses if LZ4 flag set.
 * Returns decompressed size on success, -1 on error. */
int cpk_extract(int handle, int index, void *buf, int buf_size);

/* Close a CPK handle and free resources. */
void cpk_close(int handle);

#endif /* __AIOS_KERNEL__ */
