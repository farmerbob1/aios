/* mkfs_chaos — Host-side ChaosFS format tool
 * Usage: mkfs_chaos <disk_image> <lba_start>
 * Writes a formatted ChaosFS at the given LBA offset in the disk image. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/chaos/chaos_types.h"

/* CRC-32 (same algorithm as kernel) */
static uint32_t chaos_crc32(const void* data, size_t len) {
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

static int write_block(FILE* f, uint32_t lba_start, uint32_t block_idx, const void* data) {
    long offset = (long)((lba_start + block_idx * CHAOS_SECTORS_PER_BLK) * 512);
    if (fseek(f, offset, SEEK_SET) != 0) return -1;
    if (fwrite(data, CHAOS_BLOCK_SIZE, 1, f) != 1) return -1;
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk_image> <lba_start>\n", argv[0]);
        return 1;
    }

    const char* image_path = argv[1];
    uint32_t lba_start = (uint32_t)atoi(argv[2]);

    FILE* f = fopen(image_path, "r+b");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", image_path);
        return 1;
    }

    /* Determine FS size from file size minus LBA offset */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    long fs_offset = (long)(lba_start * 512);
    if (fs_offset >= file_size) {
        fprintf(stderr, "LBA start %u exceeds file size\n", lba_start);
        fclose(f);
        return 1;
    }

    uint32_t lba_count = (uint32_t)((file_size - fs_offset) / 512);
    uint32_t total_blocks = (lba_count * 512) / CHAOS_BLOCK_SIZE;

    if (total_blocks < 64) {
        fprintf(stderr, "FS region too small: %u blocks (need >= 64)\n", total_blocks);
        fclose(f);
        return 1;
    }

    /* Compute layout */
    uint32_t bitmap_blocks = (total_blocks + 32767) / 32768;
    uint32_t inode_count = total_blocks / 4;
    if (inode_count < 32) inode_count = 32;
    uint32_t inode_table_blocks = (inode_count + CHAOS_INODES_PER_BLOCK - 1) / CHAOS_INODES_PER_BLOCK;
    uint32_t data_start = 2 + bitmap_blocks + inode_table_blocks;

    printf("mkfs_chaos: %u blocks, bitmap=%u, inodes=%u (%u blocks), data_start=%u\n",
           total_blocks, bitmap_blocks, inode_count, inode_table_blocks, data_start);

    /* Zero metadata region */
    uint8_t* zero_block = (uint8_t*)calloc(1, CHAOS_BLOCK_SIZE);
    for (uint32_t b = 0; b < data_start; b++) {
        write_block(f, lba_start, b, zero_block);
    }

    /* Build bitmap */
    size_t bitmap_size = (size_t)bitmap_blocks * CHAOS_BLOCK_SIZE;
    uint8_t* bitmap = (uint8_t*)calloc(1, bitmap_size);

    /* Mark metadata blocks as used */
    for (uint32_t b = 0; b < data_start; b++) {
        ((uint32_t*)bitmap)[b / 32] |= (1U << (b % 32));
    }

    /* Mark root directory data block */
    uint32_t root_data_block = data_start;
    ((uint32_t*)bitmap)[root_data_block / 32] |= (1U << (root_data_block % 32));

    /* Write bitmap */
    for (uint32_t i = 0; i < bitmap_blocks; i++) {
        write_block(f, lba_start, 2 + i, bitmap + i * CHAOS_BLOCK_SIZE);
    }
    free(bitmap);

    /* Create root inode (inode 1) */
    uint8_t* inode_block = (uint8_t*)calloc(1, CHAOS_BLOCK_SIZE);
    struct chaos_inode root_ino;
    memset(&root_ino, 0, sizeof(root_ino));
    root_ino.magic = CHAOS_INODE_MAGIC;
    root_ino.mode = CHAOS_TYPE_DIR | 0755;
    root_ino.link_count = 2;
    root_ino.size = CHAOS_BLOCK_SIZE;
    root_ino.extent_count = 1;
    root_ino.extents[0].start_block = root_data_block;
    root_ino.extents[0].block_count = 1;

    memcpy(inode_block + 1 * CHAOS_INODE_SIZE, &root_ino, CHAOS_INODE_SIZE);
    write_block(f, lba_start, 2 + bitmap_blocks, inode_block);
    free(inode_block);

    /* Create root directory data block (. and ..) */
    uint8_t* dir_block = (uint8_t*)calloc(1, CHAOS_BLOCK_SIZE);
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

    write_block(f, lba_start, root_data_block, dir_block);
    free(dir_block);

    /* Build superblock */
    struct chaos_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = CHAOS_MAGIC;
    sb.version = CHAOS_VERSION;
    strncpy(sb.fs_name, "AIOS", 15);
    sb.block_size = CHAOS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.bitmap_start = 2;
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_table_start = 2 + bitmap_blocks;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_start = data_start;
    sb.total_inodes = inode_count;
    sb.free_blocks = total_blocks - data_start - 1;
    sb.free_inodes = inode_count - 2;
    sb.clean_unmount = 1;
    sb.mounted = 0;

    /* Compute CRC */
    sb.checksum = chaos_crc32(&sb, (size_t)((uint8_t*)&sb.checksum - (uint8_t*)&sb));

    /* Write superblock to block 0 and block 1 */
    uint8_t* sb_block = (uint8_t*)calloc(1, CHAOS_BLOCK_SIZE);
    memcpy(sb_block, &sb, sizeof(sb));
    write_block(f, lba_start, 0, sb_block);
    write_block(f, lba_start, 1, sb_block);
    free(sb_block);
    free(zero_block);

    fclose(f);
    printf("mkfs_chaos: format complete (%u blocks, %u free, %u inodes)\n",
           total_blocks, sb.free_blocks, sb.total_inodes);
    return 0;
}
