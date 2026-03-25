/* AIOS v2 — ATA Bus Master DMA Driver
 * Uses PCI IDE controller's Bus Master Interface for DMA disk transfers.
 * Falls back gracefully — if DMA init fails, PIO continues as before. */

#include "ata_dma.h"
#include "ata.h"
#include "pci.h"
#include "serial.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/irq.h"

/* ── PCI IDE class ──────────────────────────────────── */

#define PCI_CLASS_STORAGE     0x01
#define PCI_SUBCLASS_IDE      0x01

/* ── Bus Master Interface register offsets ──────────── */

#define BMI_CMD_PRIMARY       0x00
#define BMI_STATUS_PRIMARY    0x02
#define BMI_PRD_PRIMARY       0x04

/* BMI Command register bits */
#define BMI_CMD_START         0x01
#define BMI_CMD_WRITE         0x08  /* 0 = disk→RAM, 1 = RAM→disk */

/* BMI Status register bits */
#define BMI_STATUS_ACTIVE     0x01
#define BMI_STATUS_ERROR      0x02
#define BMI_STATUS_IRQ        0x04

/* ── ATA command ports (same as ata.c) ──────────────── */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS_CMD  0x1F7
#define ATA_CONTROL     0x3F6

#define ATA_CMD_READ_DMA  0xC8
#define ATA_CMD_WRITE_DMA 0xCA

/* ── PRD (Physical Region Descriptor) ───────────────── */

typedef struct {
    uint32_t phys_addr;
    uint16_t byte_count;  /* 0 = 64KB */
    uint16_t flags;       /* bit 15 = EOT */
} __attribute__((packed)) prd_entry_t;

#define PRD_FLAG_EOT  0x8000

/* ── DMA state ──────────────────────────────────────── */

#define DMA_BUFFER_PAGES  16  /* 64KB */

static uint16_t    bmi_base = 0;
static bool        dma_avail = false;

static prd_entry_t* prd_table = NULL;
static uint32_t     prd_table_phys = 0;
static uint8_t*     dma_buffer = NULL;
static uint32_t     dma_buffer_phys = 0;

/* ── IRQ14 handler ──────────────────────────────────── */

static void ata_dma_irq_handler(void) {
    /* Acknowledge IRQ on BMI side (write 1 to clear IRQ bit) */
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ);
    /* Read ATA status register to deassert drive IRQ14 */
    inb(ATA_STATUS_CMD);
}

/* ── Wait for DMA completion (always poll — completes in microseconds) */

static int dma_wait_completion(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t status = inb(bmi_base + BMI_STATUS_PRIMARY);
        if (status & BMI_STATUS_ERROR) {
            outb(bmi_base + BMI_CMD_PRIMARY, 0);
            return -1;
        }
        if (!(status & BMI_STATUS_ACTIVE) && (status & BMI_STATUS_IRQ)) {
            outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ);
            inb(ATA_STATUS_CMD);
            return 0;
        }
    }
    outb(bmi_base + BMI_CMD_PRIMARY, 0);
    serial_printf("[ATA DMA] Polling timeout\n");
    return -1;
}

/* ── Init ───────────────────────────────────────────── */

init_result_t ata_dma_init(void) {
    /* Step 1: Find PCI IDE controller (class 0x01, subclass 0x01) */
    struct pci_device ide_dev;
    if (pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, &ide_dev) < 0) {
        serial_printf("[ATA DMA] No PCI IDE controller found\n");
        return INIT_FAIL;
    }

    /* Step 2: Read BAR4 — Bus Master Interface I/O base */
    uint32_t bar4 = ide_dev.bar[4];
    if ((bar4 & 0x01) == 0 || (bar4 & 0xFFFC) == 0) {
        serial_printf("[ATA DMA] BAR4 invalid or not I/O: 0x%x\n", bar4);
        return INIT_FAIL;
    }
    bmi_base = (uint16_t)(bar4 & 0xFFFC);
    serial_printf("[ATA DMA] BMI base: 0x%x\n", (uint32_t)bmi_base);

    /* Step 3: Enable PCI Bus Mastering */
    pci_enable_bus_mastering(&ide_dev);

    /* Step 4: Allocate PRD table (one page, 4KB, physically contiguous) */
    prd_table_phys = pmm_alloc_page();
    if (prd_table_phys == 0) {
        serial_printf("[ATA DMA] Failed to allocate PRD table page\n");
        return INIT_FAIL;
    }
    vmm_map_page(prd_table_phys, prd_table_phys, PTE_PRESENT | PTE_WRITABLE);
    prd_table = (prd_entry_t*)prd_table_phys;
    memset(prd_table, 0, 4096);

    /* Step 5: Allocate 64KB DMA bounce buffer (16 contiguous pages) */
    dma_buffer_phys = pmm_alloc_pages(DMA_BUFFER_PAGES);
    if (dma_buffer_phys == 0) {
        serial_printf("[ATA DMA] Failed to allocate DMA buffer\n");
        pmm_free_page(prd_table_phys);
        return INIT_FAIL;
    }
    vmm_map_range(dma_buffer_phys, dma_buffer_phys,
                  DMA_BUFFER_PAGES * 4096, PTE_PRESENT | PTE_WRITABLE);
    dma_buffer = (uint8_t*)dma_buffer_phys;

    /* Step 6: Clear nIEN — ensure ATA drive interrupts are enabled.
     * UEFI firmware may leave nIEN=1 (interrupts disabled) after
     * ExitBootServices(), which prevents DMA completion detection. */
    outb(ATA_CONTROL, 0x00);

    /* Step 7: Register IRQ14 handler */
    irq_register_handler(14, ata_dma_irq_handler);

    /* Step 8: Clear BMI status register */
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ | BMI_STATUS_ERROR);

    dma_avail = true;
    serial_printf("[ATA DMA] DMA ready (PRD=0x%x, buf=0x%x)\n",
                  prd_table_phys, dma_buffer_phys);
    return INIT_OK;
}

/* ── Public query ───────────────────────────────────── */

bool ata_dma_available(void) {
    return dma_avail;
}

/* ── DMA Read ───────────────────────────────────────── */

int ata_dma_read(uint32_t lba, uint32_t count, void* buffer) {
    if (!dma_avail || count == 0 || count > 128 || !buffer) return -1;
    uint32_t byte_count = count * 512;

    /* Set up PRD — single entry pointing to bounce buffer */
    prd_table[0].phys_addr = dma_buffer_phys;
    prd_table[0].byte_count = (byte_count == 65536) ? 0 : (uint16_t)byte_count;
    prd_table[0].flags = PRD_FLAG_EOT;

    /* Stop any in-progress DMA */
    outb(bmi_base + BMI_CMD_PRIMARY, 0);

    /* Clear status */
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ | BMI_STATUS_ERROR);

    /* Load PRD table address */
    outl(bmi_base + BMI_PRD_PRIMARY, prd_table_phys);

    /* Set direction = read (disk → memory) */
    outb(bmi_base + BMI_CMD_PRIMARY, 0);

    /* Issue ATA READ DMA command */
    ata_wait_bsy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR, 0x00);  /* features = 0 */
    outb(ATA_SECTOR_CNT, (uint8_t)count);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_STATUS_CMD, ATA_CMD_READ_DMA);

    /* Start DMA engine */
    outb(bmi_base + BMI_CMD_PRIMARY, BMI_CMD_START);

    /* Wait for completion */
    int ret = dma_wait_completion();

    /* Stop DMA engine */
    outb(bmi_base + BMI_CMD_PRIMARY, 0);

    /* Clear status */
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ | BMI_STATUS_ERROR);

    if (ret != 0) return -1;

    /* Copy from bounce buffer to caller */
    memcpy(buffer, dma_buffer, byte_count);
    return 0;
}

/* ── DMA Write ──────────────────────────────────────── */

int ata_dma_write(uint32_t lba, uint32_t count, const void* buffer) {
    if (!dma_avail || count == 0 || count > 128 || !buffer) return -1;
    uint32_t byte_count = count * 512;

    /* Copy caller's data into bounce buffer */
    memcpy(dma_buffer, buffer, byte_count);

    /* Set up PRD */
    prd_table[0].phys_addr = dma_buffer_phys;
    prd_table[0].byte_count = (byte_count == 65536) ? 0 : (uint16_t)byte_count;
    prd_table[0].flags = PRD_FLAG_EOT;

    /* Stop, clear, load PRD */
    outb(bmi_base + BMI_CMD_PRIMARY, 0);
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ | BMI_STATUS_ERROR);
    outl(bmi_base + BMI_PRD_PRIMARY, prd_table_phys);

    /* Set direction = write (memory → disk) */
    outb(bmi_base + BMI_CMD_PRIMARY, BMI_CMD_WRITE);

    /* Issue ATA WRITE DMA command */
    ata_wait_bsy();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_ERROR, 0x00);
    outb(ATA_SECTOR_CNT, (uint8_t)count);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_STATUS_CMD, ATA_CMD_WRITE_DMA);

    /* Start DMA engine */
    outb(bmi_base + BMI_CMD_PRIMARY, BMI_CMD_START | BMI_CMD_WRITE);

    /* Wait for completion */
    int ret = dma_wait_completion();

    /* Stop + clear */
    outb(bmi_base + BMI_CMD_PRIMARY, 0);
    outb(bmi_base + BMI_STATUS_PRIMARY, BMI_STATUS_IRQ | BMI_STATUS_ERROR);

    return ret;
}
