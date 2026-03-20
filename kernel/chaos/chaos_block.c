/* AIOS v2 — ChaosFS Block I/O Layer
 * All filesystem disk access goes through here. Never call ATA directly
 * from filesystem logic. Each block = 8 sectors = 4096 bytes. */

#include "chaos_types.h"
#include "../../drivers/ata.h"
#include "../../drivers/serial.h"

static uint32_t fs_lba_start;
static bool block_io_ready = false;

void chaos_block_set_lba(uint32_t lba_start) {
    fs_lba_start = lba_start;
    block_io_ready = true;
}

int chaos_block_read(uint32_t block_idx, void* buffer) {
    if (!block_io_ready || !buffer) return CHAOS_ERR_IO;
    uint32_t lba = fs_lba_start + (block_idx * CHAOS_SECTORS_PER_BLK);
    if (ata_read_sectors(lba, CHAOS_SECTORS_PER_BLK, buffer) != 0) {
        serial_printf("[chaosfs] block_read failed: block %u (LBA %u)\n", block_idx, lba);
        return CHAOS_ERR_IO;
    }
    return CHAOS_OK;
}

int chaos_block_write(uint32_t block_idx, const void* buffer) {
    if (!block_io_ready || !buffer) return CHAOS_ERR_IO;
    uint32_t lba = fs_lba_start + (block_idx * CHAOS_SECTORS_PER_BLK);
    if (ata_write_sectors(lba, CHAOS_SECTORS_PER_BLK, buffer) != 0) {
        serial_printf("[chaosfs] block_write failed: block %u (LBA %u)\n", block_idx, lba);
        return CHAOS_ERR_IO;
    }
    return CHAOS_OK;
}
