/* AIOS v2 — ATA/IDE PIO Disk Driver (Phase 3 STUB) */

#include "ata.h"

init_result_t ata_init(void) { return INIT_OK; }
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) { (void)lba; (void)count; (void)buffer; return -1; }
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) { (void)lba; (void)count; (void)buffer; return -1; }
bool ata_is_present(void) { return false; }
uint32_t ata_get_sector_count(void) { return 0; }
