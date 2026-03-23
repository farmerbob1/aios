/* AIOS v2 — ATA Bus Master DMA Driver */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t ata_dma_init(void);
bool ata_dma_available(void);
int  ata_dma_read(uint32_t lba, uint32_t count, void* buffer);
int  ata_dma_write(uint32_t lba, uint32_t count, const void* buffer);
