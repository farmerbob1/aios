/* AIOS v2 — ChaosFS Format
 * Implements the 9-step format sequence and CRC-32 computation. */

#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../kernel/heap.h"

/* Extern from chaos_block.c */
extern void chaos_block_set_lba(uint32_t lba_start);
extern int  chaos_block_read(uint32_t block_idx, void* buffer);
extern int  chaos_block_write(uint32_t block_idx, const void* buffer);

/* ── CRC-32 (ISO-HDLC, polynomial 0xEDB88320, reflected) ── */

uint32_t chaos_crc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

/* ── Format ────────────────────────────────────────── */

int chaos_format(uint32_t lba_start, uint32_t lba_count, const char* label) {
    /* Step 1: Validate */
    uint32_t total_blocks = (lba_count * 512) / CHAOS_BLOCK_SIZE;
    if (total_blocks < 64) return CHAOS_ERR_INVALID;

    chaos_block_set_lba(lba_start);

    /* Step 2: Compute layout */
    uint32_t bitmap_blocks = (total_blocks + 32767) / 32768;
    uint32_t inode_count = total_blocks / 4;
    if (inode_count < 32) inode_count = 32;
    uint32_t inode_table_blocks = (inode_count + CHAOS_INODES_PER_BLOCK - 1) / CHAOS_INODES_PER_BLOCK;
    uint32_t data_start = 2 + bitmap_blocks + inode_table_blocks;

    serial_printf("[chaosfs] format: %u blocks, bitmap=%u, inodes=%u (%u blocks), data_start=%u\n",
                  total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start);

    /* Step 3: Zero metadata blocks */
    uint8_t* zero_block = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!zero_block) return CHAOS_ERR_NO_SPACE;

    for (uint32_t b = 0; b < data_start; b++) {
        if (chaos_block_write(b, zero_block) != CHAOS_OK) {
            kfree(zero_block);
            return CHAOS_ERR_IO;
        }
    }

    /* Step 4: Build superblock */
    struct chaos_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = CHAOS_MAGIC;
    sb.version = CHAOS_VERSION;
    if (label) {
        strncpy(sb.fs_name, label, 15);
        sb.fs_name[15] = '\0';
    }
    sb.block_size = CHAOS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.bitmap_start = 2;
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_table_start = 2 + bitmap_blocks;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_start = data_start;
    sb.total_inodes = inode_count;
    sb.free_blocks = total_blocks - data_start - 1;  /* -1 for root dir data block */
    sb.free_inodes = inode_count - 2;  /* -1 for reserved slot 0, -1 for root inode 1 */
    sb.clean_unmount = 1;
    sb.mounted = 0;
    sb.mount_count = 0;

    /* Step 5: Build bitmap — mark metadata + root dir data block as used */
    uint8_t* bitmap_buf = (uint8_t*)kzmalloc(bitmap_blocks * CHAOS_BLOCK_SIZE);
    if (!bitmap_buf) { kfree(zero_block); return CHAOS_ERR_NO_SPACE; }

    /* Mark blocks 0..data_start-1 as used (metadata) */
    for (uint32_t b = 0; b < data_start; b++) {
        uint32_t word = b / 32;
        uint32_t bit = b % 32;
        ((uint32_t*)bitmap_buf)[word] |= (1U << bit);
    }

    /* Mark root directory data block (first data block) as used */
    uint32_t root_data_block = data_start;
    {
        uint32_t word = root_data_block / 32;
        uint32_t bit = root_data_block % 32;
        ((uint32_t*)bitmap_buf)[word] |= (1U << bit);
    }

    /* Write bitmap blocks to disk */
    for (uint32_t i = 0; i < bitmap_blocks; i++) {
        if (chaos_block_write(sb.bitmap_start + i,
                               bitmap_buf + i * CHAOS_BLOCK_SIZE) != CHAOS_OK) {
            kfree(bitmap_buf); kfree(zero_block);
            return CHAOS_ERR_IO;
        }
    }
    kfree(bitmap_buf);

    /* Step 6: Inode table already zeroed in step 3 */

    /* Step 7: Create root directory (inode 1) */
    struct chaos_inode root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.magic = CHAOS_INODE_MAGIC;
    root_ino.mode = CHAOS_TYPE_DIR | 0755;
    root_ino.link_count = 2;  /* . and parent (root's parent is itself) */
    root_ino.size = CHAOS_BLOCK_SIZE;
    root_ino.extent_count = 1;
    root_ino.extents[0].start_block = root_data_block;
    root_ino.extents[0].block_count = 1;

    /* Write root inode to inode table (inode 1 = slot 1 in block 0 of inode table) */
    uint8_t* inode_block = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!inode_block) { kfree(zero_block); return CHAOS_ERR_NO_SPACE; }

    memcpy(inode_block + 1 * CHAOS_INODE_SIZE, &root_ino, CHAOS_INODE_SIZE);
    if (chaos_block_write(sb.inode_table_start, inode_block) != CHAOS_OK) {
        kfree(inode_block); kfree(zero_block);
        return CHAOS_ERR_IO;
    }
    kfree(inode_block);

    /* Write root directory entries (. and ..) */
    uint8_t* dir_block = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!dir_block) { kfree(zero_block); return CHAOS_ERR_NO_SPACE; }

    struct chaos_dirent* d0 = (struct chaos_dirent*)dir_block;
    d0->inode = 1;
    d0->type = CHAOS_DT_DIR;
    d0->name_len = 1;
    strcpy(d0->name, ".");

    struct chaos_dirent* d1 = (struct chaos_dirent*)(dir_block + CHAOS_DIRENT_SIZE);
    d1->inode = 1;
    d1->type = CHAOS_DT_DIR;
    d1->name_len = 2;
    strcpy(d1->name, "..");

    if (chaos_block_write(root_data_block, dir_block) != CHAOS_OK) {
        kfree(dir_block); kfree(zero_block);
        return CHAOS_ERR_IO;
    }
    kfree(dir_block);
    kfree(zero_block);

    /* Step 8: Compute CRC and write superblock to block 0 and block 1 */
    sb.checksum = chaos_crc32(&sb, (size_t)((uint8_t*)&sb.checksum - (uint8_t*)&sb));

    uint8_t* sb_block = (uint8_t*)kzmalloc(CHAOS_BLOCK_SIZE);
    if (!sb_block) return CHAOS_ERR_NO_SPACE;
    memcpy(sb_block, &sb, sizeof(sb));

    if (chaos_block_write(0, sb_block) != CHAOS_OK ||
        chaos_block_write(1, sb_block) != CHAOS_OK) {
        kfree(sb_block);
        return CHAOS_ERR_IO;
    }

    /* Step 9: Verify — read back and check */
    memset(sb_block, 0, CHAOS_BLOCK_SIZE);
    if (chaos_block_read(0, sb_block) != CHAOS_OK) { kfree(sb_block); return CHAOS_ERR_IO; }

    struct chaos_superblock* verify = (struct chaos_superblock*)sb_block;
    if (verify->magic != CHAOS_MAGIC) {
        serial_print("[chaosfs] format verify failed: bad magic\n");
        kfree(sb_block);
        return CHAOS_ERR_CORRUPT;
    }

    uint32_t verify_crc = chaos_crc32(verify, (size_t)((uint8_t*)&verify->checksum - (uint8_t*)verify));
    if (verify_crc != verify->checksum) {
        serial_print("[chaosfs] format verify failed: CRC mismatch\n");
        kfree(sb_block);
        return CHAOS_ERR_CORRUPT;
    }

    kfree(sb_block);
    serial_print("[chaosfs] format complete\n");
    return CHAOS_OK;
}
