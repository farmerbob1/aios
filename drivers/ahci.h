/* AIOS v2 — AHCI/SATA Disk Driver
 * Fallback when legacy IDE is not available (e.g., VirtualBox UEFI). */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t ahci_init(void);
int      ahci_read_sectors(uint32_t lba, uint32_t count, void* buffer);
int      ahci_write_sectors(uint32_t lba, uint32_t count, const void* buffer);
bool     ahci_is_present(void);
uint32_t ahci_get_sector_count(void);
