/* AIOS v2 — ATA/IDE PIO Disk Driver (Phase 3)
 * Primary bus, drive 0, 28-bit LBA, PIO mode.
 * Used by ChaosFS for all disk I/O. */

#include "ata.h"
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
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7

#define ATA_MAX_RETRIES  3
#define ATA_TIMEOUT      400000

/* ── State ─────────────────────────────────────────── */

static bool     ata_present_val = false;
static uint32_t ata_sectors = 0;

/* ── Helpers ───────────────────────────────────────── */

static void ata_400ns_delay(void) {
    /* Reading status register takes ~100ns. 4 reads ≈ 400ns. */
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
    inb(ATA_STATUS_CMD);
}

static bool ata_wait_bsy(void) {
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

static void ata_soft_reset(void) {
    outb(ATA_CONTROL, 0x04);  /* set SRST */
    io_wait(); io_wait(); io_wait(); io_wait();
    outb(ATA_CONTROL, 0x00);  /* clear SRST */
    ata_wait_bsy();
}

static void ata_setup_lba(uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));  /* master, LBA, bits 24-27 */
    outb(ATA_SECTOR_CNT, count);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
}

/* ── Init ──────────────────────────────────────────── */

init_result_t ata_init(void) {
    /* Select drive 0 */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_400ns_delay();

    /* Check for floating bus (no drive) */
    uint8_t s = inb(ATA_STATUS_CMD);
    if (s == 0xFF) {
        serial_print("  ATA: no drive detected (floating bus)\n");
        return INIT_WARN;
    }

    /* Send IDENTIFY command */
    outb(ATA_SECTOR_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_STATUS_CMD, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    /* Check if drive responded */
    s = inb(ATA_STATUS_CMD);
    if (s == 0) {
        serial_print("  ATA: no drive (status=0)\n");
        return INIT_WARN;
    }

    /* Wait for BSY to clear */
    if (!ata_wait_bsy()) {
        serial_print("  ATA: IDENTIFY timeout\n");
        return INIT_FAIL;
    }

    /* Check for non-ATA drive (ATAPI/SATA) */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
        serial_print("  ATA: non-ATA device (ATAPI?)\n");
        return INIT_FAIL;
    }

    /* Wait for DRQ */
    if (!ata_wait_drq()) {
        serial_print("  ATA: IDENTIFY DRQ failed\n");
        return INIT_FAIL;
    }

    /* Read 256 words of IDENTIFY data */
    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(ATA_DATA);
    }

    /* Extract 28-bit LBA sector count (words 60-61) */
    ata_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    ata_present_val = true;

    serial_printf("  ATA: %u sectors (%u MB)\n", ata_sectors, ata_sectors / 2048);

    return INIT_OK;
}

/* ── Read ──────────────────────────────────────────── */

int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (!ata_present_val || count == 0 || !buffer) return -1;
    if (count > 256) return -1;
    if (lba + count > ata_sectors) return -1;

    uint16_t* p = (uint16_t*)buffer;

    for (int retry = 0; retry < ATA_MAX_RETRIES; retry++) {
        if (!ata_wait_bsy()) { ata_soft_reset(); continue; }

        ata_setup_lba(lba, (uint8_t)count);
        outb(ATA_STATUS_CMD, ATA_CMD_READ);

        bool ok = true;
        for (uint32_t sec = 0; sec < count; sec++) {
            ata_400ns_delay();
            if (!ata_wait_drq()) { ok = false; break; }

            uint16_t* dst = p + sec * 256;
            for (int i = 0; i < 256; i++) {
                dst[i] = inw(ATA_DATA);
            }
        }

        if (ok) return 0;
        ata_soft_reset();
    }

    return -1;
}

/* ── Write ─────────────────────────────────────────── */

int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (!ata_present_val || count == 0 || !buffer) return -1;
    if (count > 256) return -1;
    if (lba + count > ata_sectors) return -1;

    const uint16_t* p = (const uint16_t*)buffer;

    for (int retry = 0; retry < ATA_MAX_RETRIES; retry++) {
        if (!ata_wait_bsy()) { ata_soft_reset(); continue; }

        ata_setup_lba(lba, (uint8_t)count);
        outb(ATA_STATUS_CMD, ATA_CMD_WRITE);

        bool ok = true;
        for (uint32_t sec = 0; sec < count; sec++) {
            ata_400ns_delay();
            if (!ata_wait_drq()) { ok = false; break; }

            const uint16_t* src = p + sec * 256;
            for (int i = 0; i < 256; i++) {
                outw(ATA_DATA, src[i]);
            }
        }

        if (!ok) { ata_soft_reset(); continue; }

        /* Cache flush */
        outb(ATA_STATUS_CMD, ATA_CMD_FLUSH);
        if (!ata_wait_bsy()) { ata_soft_reset(); continue; }

        return 0;
    }

    return -1;
}

/* ── Queries ───────────────────────────────────────── */

bool ata_is_present(void) {
    return ata_present_val;
}

uint32_t ata_get_sector_count(void) {
    return ata_sectors;
}
