/* AIOS v2 — ChaosFS On-Disk Structures and Constants
 * Single source of truth for all filesystem types.
 * Used by both kernel code and the host-side format tool. */

#pragma once

#ifdef __AIOS_KERNEL__
#include "../../include/types.h"
#else
#include <stdint.h>
#include <stdbool.h>
#ifndef __GNUC__
#define __attribute__(x)
#endif
#endif

/* ── Magic numbers and version ─────────────────────── */

#define CHAOS_MAGIC           0x43484653  /* 'CHFS' */
#define CHAOS_VERSION         2
#define CHAOS_BLOCK_SIZE      4096
#define CHAOS_SECTORS_PER_BLK 8

/* ── Null sentinels ────────────────────────────────── */

#define CHAOS_BLOCK_NULL      0
#define CHAOS_INODE_NULL      0

/* ── Inode constants ───────────────────────────────── */

#define CHAOS_INODE_MAGIC        0xC4A0
#define CHAOS_INODE_SIZE         128
#define CHAOS_MAX_INLINE_EXTENTS 6
#define CHAOS_INODES_PER_BLOCK   (CHAOS_BLOCK_SIZE / CHAOS_INODE_SIZE)  /* 32 */

/* ── Type masks ────────────────────────────────────── */

#define CHAOS_TYPE_FILE    0x1000
#define CHAOS_TYPE_DIR     0x2000
#define CHAOS_TYPE_SYMLINK 0x3000  /* reserved, not implemented */
#define CHAOS_TYPE_MASK    0xF000

/* ── Directory entry constants ─────────────────────── */

#define CHAOS_DIRENT_SIZE      64
#define CHAOS_MAX_FILENAME     53
#define CHAOS_DIRENTS_PER_BLK  (CHAOS_BLOCK_SIZE / CHAOS_DIRENT_SIZE)  /* 64 */
#define CHAOS_DT_FILE          1
#define CHAOS_DT_DIR           2

/* ── Table limits ──────────────────────────────────── */

#define CHAOS_MAX_FD           16
#define CHAOS_MAX_DIR_HANDLES  8

/* ── Open flags ────────────────────────────────────── */

#define CHAOS_O_RDONLY  0x01
#define CHAOS_O_WRONLY  0x02
#define CHAOS_O_RDWR    0x03
#define CHAOS_O_CREAT   0x04
#define CHAOS_O_TRUNC   0x08
#define CHAOS_O_APPEND  0x10

/* ── Seek whence ───────────────────────────────────── */

#define CHAOS_SEEK_SET  0
#define CHAOS_SEEK_CUR  1
#define CHAOS_SEEK_END  2

/* ── FD flags ──────────────────────────────────────── */

typedef enum {
    FD_FLAG_READ   = 0x01,
    FD_FLAG_WRITE  = 0x02,
    FD_FLAG_APPEND = 0x04,
} fd_flags_t;

/* ── Error codes ───────────────────────────────────── */

typedef enum {
    CHAOS_OK              =  0,
    CHAOS_ERR_NOT_FOUND   = -1,
    CHAOS_ERR_NOT_DIR     = -2,
    CHAOS_ERR_NOT_FILE    = -3,
    CHAOS_ERR_EXISTS      = -4,
    CHAOS_ERR_NO_SPACE    = -5,
    CHAOS_ERR_NO_INODES   = -6,
    CHAOS_ERR_NO_FD       = -7,
    CHAOS_ERR_CORRUPT     = -8,
    CHAOS_ERR_IO          = -9,
    CHAOS_ERR_INVALID     = -10,
    CHAOS_ERR_NOT_EMPTY   = -11,
    CHAOS_ERR_NOT_MOUNTED = -12,
    CHAOS_ERR_TOO_LONG    = -13,
    CHAOS_ERR_BAD_FD      = -14,
    CHAOS_ERR_READ_ONLY   = -15,
} chaos_err_t;

/* ── Path limits ───────────────────────────────────── */

#define CHAOS_MAX_PATH       512
#define CHAOS_MAX_PATH_DEPTH 16

/* ── On-disk structures (all packed, little-endian) ── */

struct chaos_extent {
    uint32_t start_block;
    uint32_t block_count;
} __attribute__((packed));

struct chaos_superblock {
    /* Identity */
    uint32_t magic;
    uint32_t version;
    char     fs_name[16];

    /* Geometry — set at format, never change */
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t data_start;

    /* Dynamic counts */
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;

    /* State */
    uint8_t  clean_unmount;
    uint8_t  mounted;
    uint16_t mount_count;

    /* Timestamps (seconds since boot) */
    uint32_t created_time;
    uint32_t last_mounted_time;
    uint32_t last_fsck_time;

    /* Future extensions */
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint8_t  reserved[28];

    /* Integrity */
    uint32_t checksum;  /* CRC-32/ISO-HDLC of all preceding bytes */
} __attribute__((packed));

struct chaos_inode {
    uint16_t magic;
    uint16_t mode;
    uint32_t link_count;
    uint32_t open_count;      /* runtime-only, zeroed on load */
    bool     unlink_pending;  /* runtime-only, cleared on load */
    uint8_t  pad0[3];
    uint64_t size;
    uint32_t created_time;
    uint32_t modified_time;
    uint32_t accessed_time;
    uint32_t flags;
    uint8_t  extent_count;
    uint8_t  has_indirect;
    uint16_t reserved;
    struct chaos_extent extents[CHAOS_MAX_INLINE_EXTENTS];  /* 6 * 8 = 48 */
    uint32_t indirect_block;
    uint8_t  pad1[32];  /* fields sum to 96 bytes, pad to 128 */
} __attribute__((packed));

struct chaos_dirent {
    uint32_t inode;
    uint8_t  type;
    uint8_t  name_len;
    char     name[54];
    uint8_t  reserved[4];  /* pad to 64 bytes */
} __attribute__((packed));

/* ── Runtime-only structures ───────────────────────── */

struct chaos_stat {
    uint32_t inode;
    uint16_t mode;
    uint64_t size;
    uint32_t block_count;
    uint32_t created_time;
    uint32_t modified_time;
    uint32_t accessed_time;
};

struct chaos_fd {
    bool     in_use;
    uint32_t inode_num;
    uint64_t position;
    uint8_t  flags;
};

struct chaos_dir_handle {
    bool     in_use;
    uint32_t inode_num;
    uint32_t block_idx;    /* which extent block we're reading */
    uint32_t slot_idx;     /* slot within that block (0-63) */
};

/* ── Static asserts ────────────────────────────────── */

#ifdef __AIOS_KERNEL__
_Static_assert(sizeof(struct chaos_inode) == 128, "chaos_inode must be 128 bytes!");
_Static_assert(sizeof(struct chaos_extent) == 8,  "chaos_extent must be 8 bytes!");
_Static_assert(sizeof(struct chaos_dirent) == 64, "chaos_dirent must be 64 bytes!");
_Static_assert(sizeof(struct chaos_superblock) <= CHAOS_BLOCK_SIZE,
               "chaos_superblock exceeds one block!");
#endif
