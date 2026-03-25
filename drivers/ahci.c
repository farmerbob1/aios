/* AIOS v2 — AHCI/SATA Disk Driver
 *
 * Minimal AHCI driver for port 0, 28-bit LBA.
 * Used when legacy IDE is unavailable (e.g., VirtualBox with SATA).
 * Shares the ata_read_sectors/ata_write_sectors interface via ata.c fallback.
 */

#include "ahci.h"
#include "pci.h"
#include "serial.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"

/* ── AHCI Register Offsets (HBA level) ────────────────── */

#define HBA_CAP         0x00
#define HBA_GHC         0x04
#define HBA_IS          0x08
#define HBA_PI          0x0C
#define HBA_VS          0x10

#define HBA_GHC_AE      (1U << 31)  /* AHCI Enable */
#define HBA_GHC_HR      (1U << 0)   /* HBA Reset */

/* ── AHCI Port Register Offsets (from port base) ─────── */

#define PORT_CLB        0x00    /* Command List Base */
#define PORT_CLBU       0x04    /* Command List Base Upper */
#define PORT_FB         0x08    /* FIS Base */
#define PORT_FBU        0x0C    /* FIS Base Upper */
#define PORT_IS         0x10    /* Interrupt Status */
#define PORT_IE         0x14    /* Interrupt Enable */
#define PORT_CMD        0x18    /* Command and Status */
#define PORT_TFD        0x20    /* Task File Data */
#define PORT_SIG        0x24    /* Signature */
#define PORT_SSTS       0x28    /* SATA Status */
#define PORT_SERR       0x30    /* SATA Error */
#define PORT_CI         0x38    /* Command Issue */

#define PORT_CMD_ST     (1U << 0)   /* Start */
#define PORT_CMD_FRE    (1U << 4)   /* FIS Receive Enable */
#define PORT_CMD_FR     (1U << 14)  /* FIS Receive Running */
#define PORT_CMD_CR     (1U << 15)  /* Command List Running */

#define PORT_TFD_BSY    (1U << 7)
#define PORT_TFD_DRQ    (1U << 3)
#define PORT_TFD_ERR    (1U << 0)

#define SSTS_DET_MASK   0x0F
#define SSTS_DET_OK     0x03    /* Device present + PHY established */

/* ── FIS Types ────────────────────────────────────────── */

#define FIS_REG_H2D     0x27

/* ── ATA Commands ─────────────────────────────────────── */

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_DMA    0xC8
#define ATA_CMD_WRITE_DMA   0xCA

/* ── On-disk structures (packed, DMA-visible) ─────────── */

typedef struct {
    uint16_t flags;         /* CFL in bits 0:4, W=bit 6 */
    uint16_t prdtl;         /* PRDT entry count */
    uint32_t prdbc;         /* Bytes transferred */
    uint32_t ctba;          /* Command table phys addr */
    uint32_t ctbau;         /* Upper 32 bits (0 for us) */
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;  /* 32 bytes */

typedef struct {
    uint32_t dba;           /* Data buffer phys addr */
    uint32_t dbau;          /* Upper 32 bits (0) */
    uint32_t reserved;
    uint32_t dbc;           /* Byte count - 1 (bit 31 = IOC) */
} __attribute__((packed)) ahci_prdt_entry_t;  /* 16 bytes */

typedef struct {
    uint8_t  cfis[64];      /* Command FIS */
    uint8_t  acmd[16];      /* ATAPI command */
    uint8_t  reserved[48];
    ahci_prdt_entry_t prdt[8];  /* Up to 8 PRDT entries */
} __attribute__((packed)) ahci_cmd_table_t;

/* ── State ────────────────────────────────────────────── */

static volatile uint8_t *hba_base = 0;     /* MMIO base (BAR5) */
static volatile uint8_t *port_base = 0;    /* Port 0 registers */
static ahci_cmd_header_t *cmd_list = 0;    /* 32 command headers */
static ahci_cmd_table_t  *cmd_table = 0;   /* Command table for slot 0 */
static uint8_t           *fis_recv = 0;    /* FIS receive area */
static uint32_t           data_buf_phys = 0;  /* DMA bounce buffer */
static uint8_t           *data_buf = 0;
static bool               ahci_avail = false;
static uint32_t           ahci_sectors = 0;

#define DATA_BUF_PAGES  16  /* 64KB bounce buffer */

/* ── MMIO Helpers ─────────────────────────────────────── */

static inline uint32_t mmio_read(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void mmio_write(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(base + off) = val;
}

/* ── Port Control ─────────────────────────────────────── */

static void port_stop(void) {
    uint32_t cmd = mmio_read(port_base, PORT_CMD);
    cmd &= ~PORT_CMD_ST;
    mmio_write(port_base, PORT_CMD, cmd);
    /* Wait for CR to clear */
    for (int i = 0; i < 500000; i++) {
        if (!(mmio_read(port_base, PORT_CMD) & PORT_CMD_CR)) break;
    }
    cmd = mmio_read(port_base, PORT_CMD);
    cmd &= ~PORT_CMD_FRE;
    mmio_write(port_base, PORT_CMD, cmd);
    for (int i = 0; i < 500000; i++) {
        if (!(mmio_read(port_base, PORT_CMD) & PORT_CMD_FR)) break;
    }
}

static void port_start(void) {
    /* Wait for BSY/DRQ to clear */
    for (int i = 0; i < 500000; i++) {
        uint32_t tfd = mmio_read(port_base, PORT_TFD);
        if (!(tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) break;
    }
    uint32_t cmd = mmio_read(port_base, PORT_CMD);
    cmd |= PORT_CMD_FRE;
    mmio_write(port_base, PORT_CMD, cmd);
    cmd |= PORT_CMD_ST;
    mmio_write(port_base, PORT_CMD, cmd);
}

/* ── Issue Command ────────────────────────────────────── */

static int ahci_issue_cmd(uint32_t slot) {
    /* Clear port interrupt status */
    mmio_write(port_base, PORT_IS, 0xFFFFFFFF);
    mmio_write(port_base, PORT_SERR, 0xFFFFFFFF);

    /* Issue command */
    mmio_write(port_base, PORT_CI, 1U << slot);

    /* Poll for completion */
    for (int i = 0; i < 2000000; i++) {
        uint32_t ci = mmio_read(port_base, PORT_CI);
        if (!(ci & (1U << slot))) return 0;  /* Done */
        uint32_t is = mmio_read(port_base, PORT_IS);
        if (is & (1 << 30)) return -1;  /* Task File Error */
    }
    return -1;  /* Timeout */
}

/* ── Build Register H2D FIS ───────────────────────────── */

static void build_h2d_fis(uint8_t *fis, uint8_t cmd, uint32_t lba, uint16_t count) {
    memset(fis, 0, 20);
    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80;         /* Command (not control) */
    fis[2] = cmd;
    fis[3] = 0;            /* Features */
    fis[4] = (uint8_t)(lba & 0xFF);
    fis[5] = (uint8_t)((lba >> 8) & 0xFF);
    fis[6] = (uint8_t)((lba >> 16) & 0xFF);
    fis[7] = 0xE0 | ((lba >> 24) & 0x0F);  /* Device + LBA mode */
    fis[12] = (uint8_t)(count & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);
}

/* ── Init ─────────────────────────────────────────────── */

init_result_t ahci_init(void) {
    /* Find AHCI controller: PCI class 01:06 */
    struct pci_device dev;
    if (pci_find_class(0x01, 0x06, &dev) < 0) {
        return INIT_WARN;  /* No AHCI controller */
    }

    /* Read BAR5 (ABAR) */
    uint32_t bar5 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x24);
    if (bar5 == 0 || (bar5 & 1)) {
        serial_print("[AHCI] BAR5 invalid\n");
        return INIT_FAIL;
    }
    bar5 &= 0xFFFFF000;

    /* Map AHCI MMIO region (typically 8KB) */
    uint32_t mmio_size = 0x2000;
    vmm_map_range(bar5, bar5, mmio_size,
                  PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    hba_base = (volatile uint8_t*)bar5;

    serial_printf("[AHCI] HBA at 0x%08x, version %08x\n",
                  bar5, mmio_read(hba_base, HBA_VS));

    /* Enable AHCI mode */
    uint32_t ghc = mmio_read(hba_base, HBA_GHC);
    ghc |= HBA_GHC_AE;
    mmio_write(hba_base, HBA_GHC, ghc);

    /* Check port 0 is implemented */
    uint32_t pi = mmio_read(hba_base, HBA_PI);
    if (!(pi & 1)) {
        serial_print("[AHCI] Port 0 not implemented\n");
        return INIT_FAIL;
    }

    port_base = hba_base + 0x100;  /* Port 0 offset */

    /* Check device present */
    uint32_t ssts = mmio_read(port_base, PORT_SSTS);
    if ((ssts & SSTS_DET_MASK) != SSTS_DET_OK) {
        serial_print("[AHCI] No device on port 0\n");
        return INIT_FAIL;
    }

    /* Enable PCI bus mastering */
    uint32_t pci_cmd = pci_config_read32(dev.bus, dev.slot, dev.func, 0x04);
    pci_cmd |= (1 << 2);  /* Bus Master Enable */
    pci_config_write32(dev.bus, dev.slot, dev.func, 0x04, pci_cmd);

    /* Allocate DMA memory: command list (1KB), FIS recv (256B), cmd table (256B) */
    uint32_t cl_phys = pmm_alloc_page();
    uint32_t fb_phys = pmm_alloc_page();
    uint32_t ct_phys = pmm_alloc_page();
    if (!cl_phys || !fb_phys || !ct_phys) {
        serial_print("[AHCI] Cannot allocate DMA memory\n");
        return INIT_FAIL;
    }
    vmm_map_page(cl_phys, cl_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    vmm_map_page(fb_phys, fb_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    vmm_map_page(ct_phys, ct_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);

    memset((void*)cl_phys, 0, 4096);
    memset((void*)fb_phys, 0, 4096);
    memset((void*)ct_phys, 0, 4096);

    cmd_list  = (ahci_cmd_header_t*)cl_phys;
    fis_recv  = (uint8_t*)fb_phys;
    cmd_table = (ahci_cmd_table_t*)ct_phys;

    /* Point command header slot 0 at our command table */
    cmd_list[0].ctba  = ct_phys;
    cmd_list[0].ctbau = 0;

    /* Allocate bounce buffer for DMA data */
    data_buf_phys = pmm_alloc_pages(DATA_BUF_PAGES);
    if (!data_buf_phys) {
        serial_print("[AHCI] Cannot allocate bounce buffer\n");
        return INIT_FAIL;
    }
    vmm_map_range(data_buf_phys, data_buf_phys,
                  DATA_BUF_PAGES * 4096, PTE_PRESENT | PTE_WRITABLE);
    data_buf = (uint8_t*)data_buf_phys;

    /* Stop port, configure, restart */
    port_stop();

    mmio_write(port_base, PORT_CLB, cl_phys);
    mmio_write(port_base, PORT_CLBU, 0);
    mmio_write(port_base, PORT_FB, fb_phys);
    mmio_write(port_base, PORT_FBU, 0);
    mmio_write(port_base, PORT_SERR, 0xFFFFFFFF);
    mmio_write(port_base, PORT_IS, 0xFFFFFFFF);

    port_start();

    /* IDENTIFY DEVICE */
    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));
    build_h2d_fis(cmd_table->cfis, ATA_CMD_IDENTIFY, 0, 0);
    cmd_table->prdt[0].dba  = data_buf_phys;
    cmd_table->prdt[0].dbau = 0;
    cmd_table->prdt[0].dbc  = 511;  /* 512 bytes - 1 */

    cmd_list[0].flags = (5 << 0);  /* CFL = 5 dwords (20 bytes) */
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;

    if (ahci_issue_cmd(0) != 0) {
        serial_print("[AHCI] IDENTIFY failed\n");
        return INIT_FAIL;
    }

    uint16_t *ident = (uint16_t*)data_buf;
    ahci_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    ahci_avail = true;

    serial_printf("[AHCI] %u sectors (%u MB)\n", ahci_sectors, ahci_sectors / 2048);
    return INIT_OK;
}

/* ── Read Sectors ─────────────────────────────────────── */

int ahci_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (!ahci_avail || !buffer || count == 0) return -1;
    if (count > 128) return -1;

    uint32_t byte_count = count * 512;

    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));
    build_h2d_fis(cmd_table->cfis, ATA_CMD_READ_DMA, lba, (uint16_t)count);

    cmd_table->prdt[0].dba  = data_buf_phys;
    cmd_table->prdt[0].dbau = 0;
    cmd_table->prdt[0].dbc  = byte_count - 1;

    cmd_list[0].flags = (5 << 0);  /* CFL=5, not write */
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;

    if (ahci_issue_cmd(0) != 0) return -1;

    memcpy(buffer, data_buf, byte_count);
    return 0;
}

/* ── Write Sectors ────────────────────────────────────── */

int ahci_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (!ahci_avail || !buffer || count == 0) return -1;
    if (count > 128) return -1;

    uint32_t byte_count = count * 512;
    memcpy(data_buf, buffer, byte_count);

    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));
    build_h2d_fis(cmd_table->cfis, ATA_CMD_WRITE_DMA, lba, (uint16_t)count);

    cmd_table->prdt[0].dba  = data_buf_phys;
    cmd_table->prdt[0].dbau = 0;
    cmd_table->prdt[0].dbc  = byte_count - 1;

    cmd_list[0].flags = (5 << 0) | (1 << 6);  /* CFL=5, Write bit */
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;

    return ahci_issue_cmd(0);
}

/* ── Queries ──────────────────────────────────────────── */

bool ahci_is_present(void) { return ahci_avail; }
uint32_t ahci_get_sector_count(void) { return ahci_sectors; }
