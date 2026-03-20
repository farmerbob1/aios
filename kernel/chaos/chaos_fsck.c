/* AIOS v2 — ChaosFS Consistency Checker
 * Verifies bitmap, link counts, extent validity, directory structure. */

#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../kernel/heap.h"

/* Extern from chaos modules */
extern int      chaos_block_read(uint32_t block_idx, void* buffer);
extern int      chaos_block_write(uint32_t block_idx, const void* buffer);
extern int      chaos_inode_flush(void);
extern void     chaos_inode_invalidate(void);
extern uint32_t chaos_get_data_start(void);
extern uint32_t chaos_get_total_blocks(void);
extern uint32_t chaos_get_total_inodes(void);
extern uint32_t chaos_get_inode_table_start(void);
extern uint32_t chaos_get_inode_table_blocks(void);
extern uint32_t* chaos_get_bitmap_cache(void);
extern uint32_t chaos_get_bitmap_words(void);
extern bool     chaos_is_mounted(void);

int chaos_fsck(void) {
    if (!chaos_is_mounted()) return -1;

    serial_print("[fsck] starting consistency check\n");

    /* Flush and invalidate inode cache first */
    chaos_inode_flush();
    chaos_inode_invalidate();

    int errors = 0;
    uint32_t total_blocks = chaos_get_total_blocks();
    uint32_t total_inodes = chaos_get_total_inodes();
    uint32_t data_start = chaos_get_data_start();
    uint32_t inode_table_start = chaos_get_inode_table_start();
    uint32_t inode_table_blocks = chaos_get_inode_table_blocks();

    /* Allocate a "referenced blocks" bitmap */
    uint32_t ref_words = (total_blocks + 31) / 32;
    uint32_t* ref_bitmap = (uint32_t*)kzmalloc(ref_words * 4);
    if (!ref_bitmap) { serial_print("[fsck] out of memory\n"); return -1; }

    /* Allocate link count array */
    uint16_t* link_counts = (uint16_t*)kzmalloc(total_inodes * sizeof(uint16_t));
    if (!link_counts) { kfree(ref_bitmap); serial_print("[fsck] out of memory\n"); return -1; }

    /* Mark metadata blocks as referenced */
    for (uint32_t b = 0; b < data_start; b++) {
        ref_bitmap[b / 32] |= (1U << (b % 32));
    }

    uint8_t* block_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!block_buf) { kfree(ref_bitmap); kfree(link_counts); return -1; }

    /* Walk all inodes, collect block references */
    for (uint32_t tb = 0; tb < inode_table_blocks; tb++) {
        if (chaos_block_read(inode_table_start + tb, block_buf) != CHAOS_OK) continue;

        for (uint32_t s = 0; s < CHAOS_INODES_PER_BLOCK; s++) {
            uint32_t ino_num = tb * CHAOS_INODES_PER_BLOCK + s;
            if (ino_num == 0) continue;

            struct chaos_inode* ino = (struct chaos_inode*)(block_buf + s * CHAOS_INODE_SIZE);
            if (ino->magic != CHAOS_INODE_MAGIC) continue;

            /* Check extent validity and mark blocks */
            for (int e = 0; e < ino->extent_count && e < CHAOS_MAX_INLINE_EXTENTS; e++) {
                uint32_t start = ino->extents[e].start_block;
                uint32_t count = ino->extents[e].block_count;

                for (uint32_t b = 0; b < count; b++) {
                    uint32_t blk = start + b;
                    if (blk < data_start || blk >= total_blocks) {
                        serial_printf("[fsck] inode %u: invalid extent block %u\n", ino_num, blk);
                        errors++;
                        continue;
                    }
                    ref_bitmap[blk / 32] |= (1U << (blk % 32));
                }
            }

            /* If directory, walk entries to count link references */
            if ((ino->mode & CHAOS_TYPE_MASK) == CHAOS_TYPE_DIR) {
                uint8_t* dir_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
                if (!dir_buf) continue;

                uint32_t dir_blocks = 0;
                for (int e = 0; e < ino->extent_count && e < CHAOS_MAX_INLINE_EXTENTS; e++) {
                    dir_blocks += ino->extents[e].block_count;
                }

                uint32_t pos = 0;
                for (int e = 0; e < ino->extent_count && e < CHAOS_MAX_INLINE_EXTENTS; e++) {
                    for (uint32_t b = 0; b < ino->extents[e].block_count; b++) {
                        uint32_t phys = ino->extents[e].start_block + b;
                        if (chaos_block_read(phys, dir_buf) != CHAOS_OK) { pos++; continue; }

                        for (uint32_t ds = 0; ds < CHAOS_DIRENTS_PER_BLK; ds++) {
                            struct chaos_dirent* d = (struct chaos_dirent*)(dir_buf + ds * CHAOS_DIRENT_SIZE);
                            if (d->inode != CHAOS_INODE_NULL && d->inode < total_inodes) {
                                /* Don't count . and .. for link count cross-check
                                 * (they are part of the directory's own accounting) */
                                if (strcmp(d->name, ".") != 0 && strcmp(d->name, "..") != 0) {
                                    link_counts[d->inode]++;
                                }
                            }
                        }
                        pos++;
                    }
                }
                kfree(dir_buf);
            }
        }
    }

    /* Cross-check bitmap: compare ref_bitmap with actual bitmap */
    uint32_t* actual = chaos_get_bitmap_cache();
    uint32_t bitmap_words = chaos_get_bitmap_words();

    for (uint32_t w = 0; w < bitmap_words && w < ref_words; w++) {
        uint32_t ref = ref_bitmap[w];
        uint32_t act = actual[w];

        if (ref != act) {
            /* Find specific bit differences */
            uint32_t diff = ref ^ act;
            for (int bit = 0; bit < 32; bit++) {
                if (!(diff & (1U << bit))) continue;
                uint32_t blk = w * 32 + bit;
                if (blk >= total_blocks) continue;

                if ((ref & (1U << bit)) && !(act & (1U << bit))) {
                    serial_printf("[fsck] block %u: referenced but marked free — fixing\n", blk);
                    actual[w] |= (1U << bit);
                    errors++;
                } else if (!(ref & (1U << bit)) && (act & (1U << bit))) {
                    if (blk >= data_start) {
                        serial_printf("[fsck] block %u: unreferenced but marked used — freeing\n", blk);
                        actual[w] &= ~(1U << bit);
                        errors++;
                    }
                }
            }
        }
    }

    /* Cross-check link counts (for files referenced by directories) */
    for (uint32_t tb = 0; tb < inode_table_blocks; tb++) {
        if (chaos_block_read(inode_table_start + tb, block_buf) != CHAOS_OK) continue;

        for (uint32_t s = 0; s < CHAOS_INODES_PER_BLOCK; s++) {
            uint32_t ino_num = tb * CHAOS_INODES_PER_BLOCK + s;
            if (ino_num == 0 || ino_num >= total_inodes) continue;

            struct chaos_inode* ino = (struct chaos_inode*)(block_buf + s * CHAOS_INODE_SIZE);
            if (ino->magic != CHAOS_INODE_MAGIC) continue;

            /* Only check files (directories have special link count rules) */
            if ((ino->mode & CHAOS_TYPE_MASK) == CHAOS_TYPE_FILE) {
                if (ino->link_count != link_counts[ino_num]) {
                    serial_printf("[fsck] inode %u: link_count=%u but %u refs found — fixing\n",
                                  ino_num, ino->link_count, link_counts[ino_num]);
                    ino->link_count = link_counts[ino_num];
                    /* Write back fixed inode */
                    chaos_block_write(inode_table_start + tb, block_buf);
                    errors++;
                }
            }
        }
    }

    kfree(block_buf);
    kfree(ref_bitmap);
    kfree(link_counts);

    /* Invalidate cache after modifications */
    chaos_inode_invalidate();

    serial_printf("[fsck] complete: %d errors found\n", errors);
    return errors;
}
