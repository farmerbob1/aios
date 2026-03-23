/* AIOS v2 — ATA/IDE Disk Driver
 * Init via PIO IDENTIFY. All data transfers via DMA. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t ata_init(void);
int      ata_read_sectors(uint32_t lba, uint32_t count, void* buffer);
int      ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer);
bool     ata_is_present(void);
uint32_t ata_get_sector_count(void);
bool     ata_wait_bsy(void);
