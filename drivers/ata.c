/* AIOS v2 — ATA/IDE Disk Driver
 * Primary bus, drive 0, 28-bit LBA.
 * Init uses PIO for IDENTIFY. All data transfers use DMA. */

#include "ata.h"
#include "ata_dma.h"
#include "ahci.h"
#include "../include/io.h"
#include "../drivers/serial.h"

/* ── Port definitions ──────────────────────────────── */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS_CMD  0x1F7
#define ATA_CONTROL     0x3F6

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_IDENTIFY 0xEC

#define ATA_TIMEOUT      400000

/* ── State ─────────────────────────────────────────── */

static bool     ata_present_val = false;
static uint32_t ata_sectors = 0;
static bool     use_ahci = false;

/* ── Helpers ───────────────────────────────────────── */

static void ata_400ns_delay(void) {
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
}

bool ata_wait_bsy(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t s = inb(ATA_STATUS_CMD);
        if (!(s & ATA_STATUS_BSY)) return true;
    }
    return false;
}

static bool ata_wait_drq(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t s = inb(ATA_STATUS_CMD);
        if (s & ATA_STATUS_ERR) return false;
        if (s & ATA_STATUS_DRQ) return true;
    }
    return false;
}

/* ── Init (PIO IDENTIFY — only PIO usage, runs before DMA is set up) */

init_result_t ata_init(void) {
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_400ns_delay();

    uint8_t s = inb(ATA_STATUS_CMD);
    if (s == 0xFF) {
        serial_print("  ATA: no legacy IDE drive (floating bus)\n");
        return INIT_WARN;
    }

    outb(ATA_SECTOR_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_STATUS_CMD, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    s = inb(ATA_STATUS_CMD);
    if (s == 0) {
        serial_print("  ATA: no drive (status=0)\n");
        return INIT_WARN;
    }

    if (!ata_wait_bsy()) {
        serial_print("  ATA: IDENTIFY timeout\n");
        return INIT_FAIL;
    }

    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        serial_print("  ATA: non-ATA device (ATAPI?)\n");
        return INIT_FAIL;
    }

    if (!ata_wait_drq()) {
        serial_print("  ATA: IDENTIFY DRQ failed\n");
        return INIT_FAIL;
    }

    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(ATA_DATA);
    }

    ata_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    ata_present_val = true;

    serial_printf("  ATA: %u sectors (%u MB)\n", ata_sectors, ata_sectors / 2048);
    return INIT_OK;
}

/* ── Public read/write — DMA only ──────────────────── */

int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (!ata_present_val || count == 0 || !buffer) return -1;
    if (count > 128) return -1;
    if (lba + count > ata_sectors) return -1;
    if (use_ahci) return ahci_read_sectors(lba, count, buffer);
    return ata_dma_read(lba, count, buffer);
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (!ata_present_val || count == 0 || !buffer) return -1;
    if (count > 128) return -1;
    if (lba + count > ata_sectors) return -1;
    if (use_ahci) return ahci_write_sectors(lba, count, buffer);
    return ata_dma_write(lba, count, buffer);
}

/* ── AHCI fallback (call after pci_init) ──────────── */

init_result_t ata_init_ahci(void) {
    if (ata_present_val) return INIT_OK;  /* Legacy IDE already working */
    init_result_t r = ahci_init();
    if (r == INIT_OK) {
        ata_present_val = true;
        ata_sectors = ahci_get_sector_count();
        use_ahci = true;
    }
    return r;
}

/* ── Queries ───────────────────────────────────────── */

bool ata_is_present(void) {
    return ata_present_val;
}

uint32_t ata_get_sector_count(void) {
    return ata_sectors;
}
