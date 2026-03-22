/* AIOS v2 — Intel ICH AC97 Audio Driver (Phase 12)
 *
 * KAOS module for the AC97 codec emulated by QEMU.
 * PCI vendor 0x8086, device 0x2415.
 *
 * Uses I/O port access (BAR0 = mixer, BAR1 = bus master).
 * DMA via Buffer Descriptor List (32 entries, circular). */

#include <kaos/module.h>
#include <types.h>

/* ── Kernel API (resolved via KAOS symbol table) ────────── */

extern void  serial_printf(const char *fmt, ...);
extern void  serial_print(const char *str);
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern void  memset(void *dst, int c, size_t n);
extern void  memcpy(void *dst, const void *src, size_t n);

extern int   pci_find_device(uint16_t vendor, uint16_t device, void *out);
extern void  pci_enable_bus_mastering(void *dev);

extern uint32_t pmm_alloc_pages(uint32_t count);
extern void     pmm_free_pages(uint32_t addr, uint32_t count);
extern void     vmm_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);

extern void  irq_register_handler(int irq, void (*handler)(void));
extern void  irq_unmask(int irq);
extern void  irq_mask(int irq);

/* Port I/O wrappers */
extern uint8_t  kaos_inb(uint16_t port);
extern void     kaos_outb(uint16_t port, uint8_t val);
extern uint16_t kaos_inw(uint16_t port);
extern void     kaos_outw(uint16_t port, uint16_t val);
extern uint32_t kaos_inl(uint16_t port);
extern void     kaos_outl(uint16_t port, uint32_t val);
extern void     kaos_io_wait(void);

/* Audio bridge */
typedef void (*audio_fill_fn)(int16_t *buf, uint32_t sample_count);
extern int  audio_bridge_register_driver(void *fill_cb, void *info_cb);
extern audio_fill_fn audio_bridge_get_fill(void);

/* ── PCI device struct (must match drivers/pci.h) ──────── */

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
    uint8_t  irq_line, irq_pin;
    uint32_t bar[6];
    uint32_t bar_size[6];
    bool     bar_is_io[6];
};

/* ── AC97 Register Definitions ─────────────────────────── */

/* PCI IDs */
#define AC97_VENDOR     0x8086
#define AC97_DEVICE     0x2415

/* Native Audio Mixer (NAM) — offsets from BAR0 I/O base */
#define NAM_RESET           0x00
#define NAM_MASTER_VOL      0x02
#define NAM_PCM_OUT_VOL     0x18
#define NAM_EXT_AUDIO_ID    0x28
#define NAM_EXT_AUDIO_CTRL  0x2A
#define NAM_PCM_FRONT_RATE  0x2C

/* Native Audio Bus Master (NABM) — offsets from BAR1 I/O base */
/* PCM Out channel */
#define NABM_PO_BDBAR       0x10  /* Buffer Descriptor Base Address */
#define NABM_PO_CIV         0x14  /* Current Index Value (8-bit) */
#define NABM_PO_LVI         0x15  /* Last Valid Index (8-bit) */
#define NABM_PO_SR          0x16  /* Status Register (16-bit) */
#define NABM_PO_PICB        0x18  /* Position in Current Buffer (16-bit) */
#define NABM_PO_PIV         0x1A  /* Prefetch Index Value (8-bit) */
#define NABM_PO_CR          0x1B  /* Control Register (8-bit) */

/* Global */
#define NABM_GLOB_CNT       0x2C  /* Global Control (32-bit) */
#define NABM_GLOB_STA        0x30  /* Global Status (32-bit) */

/* Status Register bits */
#define SR_DCH      (1 << 0)
#define SR_CELV     (1 << 1)
#define SR_LVBCI    (1 << 2)
#define SR_BCIS     (1 << 3)
#define SR_FIFOE    (1 << 4)

/* Control Register bits */
#define CR_RPBM     (1 << 0)   /* Run/Pause bus master */
#define CR_RR       (1 << 1)   /* Reset registers */
#define CR_LVBIE    (1 << 2)   /* Last valid buffer interrupt enable */
#define CR_FEIE     (1 << 3)   /* FIFO error interrupt enable */
#define CR_IOCE     (1 << 4)   /* Interrupt on completion enable */

/* Global Control bits */
#define GC_GIE      (1 << 0)   /* GPI interrupt enable */
#define GC_COLD_RST (1 << 1)   /* Cold reset */
#define GC_WARM_RST (1 << 2)   /* Warm reset */

/* Extended Audio bits */
#define EXT_VRA     (1 << 0)   /* Variable Rate Audio */

/* ── BDL Entry ─────────────────────────────────────────── */

struct ac97_bdl_entry {
    uint32_t addr;      /* Physical address of PCM buffer */
    uint16_t length;    /* Number of samples (NOT bytes) */
    uint16_t flags;     /* Bit 15: IOC, Bit 14: BUP */
} __attribute__((packed));

#define BDL_IOC     (1 << 15)
#define BDL_BUP     (1 << 14)

/* ── Configuration ─────────────────────────────────────── */

#define NUM_BDL         32
#define BUF_SAMPLES     4096    /* Stereo samples per buffer */
#define BUF_SIZE        (BUF_SAMPLES * 2 * sizeof(int16_t))  /* 16KB per buffer */
#define PAGE_SIZE       4096

#define PTE_PRESENT     0x001
#define PTE_WRITABLE    0x002
#define PTE_NOCACHE     0x010
#define PTE_WRITETHROUGH 0x008

/* ── Module State ──────────────────────────────────────── */

static uint16_t nam_port = 0;
static uint16_t nabm_port = 0;
static uint8_t  irq_line = 0;

static struct ac97_bdl_entry *bdl = NULL;
static uint32_t bdl_phys = 0;

static int16_t *pcm_bufs = NULL;
static uint32_t pcm_bufs_phys = 0;

static audio_fill_fn fill_callback = NULL;
static volatile uint32_t irq_count = 0;

/* ── I/O Helpers ───────────────────────────────────────── */

static void nam_w16(uint16_t reg, uint16_t val) { kaos_outw(nam_port + reg, val); }
static uint16_t nam_r16(uint16_t reg) { return kaos_inw(nam_port + reg); }
static void nabm_w8(uint16_t reg, uint8_t val) { kaos_outb(nabm_port + reg, val); }
static uint8_t nabm_r8(uint16_t reg) { return kaos_inb(nabm_port + reg); }
static void nabm_w16(uint16_t reg, uint16_t val) { kaos_outw(nabm_port + reg, val); }
static uint16_t nabm_r16(uint16_t reg) { return kaos_inw(nabm_port + reg); }
static void nabm_w32(uint16_t reg, uint32_t val) { kaos_outl(nabm_port + reg, val); }

/* ── DMA Allocation ────────────────────────────────────── */

static uint32_t alloc_dma(uint32_t pages) {
    uint32_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    vmm_map_range(phys, phys, pages * PAGE_SIZE,
                  PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    memset((void *)phys, 0, pages * PAGE_SIZE);
    return phys;
}

/* ── IRQ Handler ───────────────────────────────────────── */

static void ac97_irq_handler(void) {
    uint16_t sr = nabm_r16(NABM_PO_SR);

    if (sr & SR_BCIS) {
        /* Buffer completed — acknowledge */
        nabm_w16(NABM_PO_SR, SR_BCIS);
        irq_count++;

        /* Fill the next buffer via mixer callback */
        if (fill_callback) {
            uint8_t civ = nabm_r8(NABM_PO_CIV);
            /* Fill the buffer AFTER the current one */
            uint8_t fill_idx = (civ + 1) % NUM_BDL;
            int16_t *buf = pcm_bufs + fill_idx * BUF_SAMPLES * 2;
            fill_callback(buf, BUF_SAMPLES);
        }

        /* Keep DMA running: set LVI ahead of CIV */
        uint8_t civ = nabm_r8(NABM_PO_CIV);
        uint8_t new_lvi = (civ + NUM_BDL - 1) % NUM_BDL;
        nabm_w8(NABM_PO_LVI, new_lvi);
    }

    if (sr & SR_LVBCI) {
        nabm_w16(NABM_PO_SR, SR_LVBCI);
    }

    if (sr & SR_FIFOE) {
        nabm_w16(NABM_PO_SR, SR_FIFOE);
    }
}

/* ── Init ──────────────────────────────────────────────── */

static int ac97_init(void) {
    struct pci_device dev;

    /* 1. Find AC97 on PCI bus */
    if (pci_find_device(AC97_VENDOR, AC97_DEVICE, &dev) < 0) {
        serial_print("[ac97] device not found\n");
        return -1;
    }

    /* 2. Extract I/O ports from BARs */
    if (!dev.bar_is_io[0] || !dev.bar_is_io[1]) {
        serial_print("[ac97] BARs not I/O space\n");
        return -1;
    }
    nam_port = (uint16_t)(dev.bar[0] & 0xFFFC);
    nabm_port = (uint16_t)(dev.bar[1] & 0xFFFC);
    irq_line = dev.irq_line;

    serial_printf("[ac97] PCI %02x:%02x.%x IRQ=%d NAM=0x%04x NABM=0x%04x\n",
                  dev.bus, dev.slot, dev.func, irq_line, nam_port, nabm_port);

    /* 3. Enable bus mastering for DMA */
    pci_enable_bus_mastering(&dev);

    /* 4. Cold reset */
    nabm_w32(NABM_GLOB_CNT, GC_COLD_RST);
    /* Simple delay — read a register a bunch of times */
    for (volatile int i = 0; i < 100000; i++) { (void)nabm_r8(NABM_PO_CIV); }
    nabm_w32(NABM_GLOB_CNT, GC_COLD_RST | GC_WARM_RST);
    for (volatile int i = 0; i < 100000; i++) { (void)nabm_r8(NABM_PO_CIV); }

    /* 5. Wait for codec ready */
    uint32_t timeout = 1000000;
    while (timeout-- > 0) {
        uint16_t ext_id = nam_r16(NAM_EXT_AUDIO_ID);
        if (ext_id != 0x0000 && ext_id != 0xFFFF) break;
    }
    if (timeout == 0) {
        serial_print("[ac97] codec not ready (timeout)\n");
        return -1;
    }

    /* 6. Reset codec */
    nam_w16(NAM_RESET, 0x0000);
    for (volatile int i = 0; i < 10000; i++) { (void)nabm_r8(NABM_PO_CIV); }

    /* 7. Set volumes — 0x0000 = max volume (unmuted) */
    nam_w16(NAM_MASTER_VOL, 0x0000);
    nam_w16(NAM_PCM_OUT_VOL, 0x0808);  /* Moderate PCM volume */

    /* 8. Enable Variable Rate Audio and set 48kHz */
    uint16_t ext_ctrl = nam_r16(NAM_EXT_AUDIO_CTRL);
    nam_w16(NAM_EXT_AUDIO_CTRL, ext_ctrl | EXT_VRA);
    nam_w16(NAM_PCM_FRONT_RATE, 48000);

    uint16_t actual_rate = nam_r16(NAM_PCM_FRONT_RATE);
    serial_printf("[ac97] sample rate: %u Hz\n", actual_rate);

    /* 9. Reset PCM Out channel */
    nabm_w8(NABM_PO_CR, CR_RR);
    for (volatile int i = 0; i < 10000; i++) { (void)nabm_r8(NABM_PO_CR); }
    nabm_w8(NABM_PO_CR, 0);

    /* 10. Allocate BDL (32 entries * 8 bytes = 256 bytes, fits in 1 page) */
    bdl_phys = alloc_dma(1);
    if (!bdl_phys) {
        serial_print("[ac97] BDL alloc failed\n");
        return -1;
    }
    bdl = (struct ac97_bdl_entry *)bdl_phys;

    /* 11. Allocate PCM buffers (32 * 16KB = 512KB = 128 pages) */
    uint32_t pcm_pages = (NUM_BDL * BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    pcm_bufs_phys = alloc_dma(pcm_pages);
    if (!pcm_bufs_phys) {
        serial_print("[ac97] PCM buffer alloc failed\n");
        return -1;
    }
    pcm_bufs = (int16_t *)pcm_bufs_phys;

    /* 12. Fill BDL entries */
    for (int i = 0; i < NUM_BDL; i++) {
        bdl[i].addr = pcm_bufs_phys + i * BUF_SIZE;
        bdl[i].length = BUF_SAMPLES;  /* Number of samples (stereo pairs) */
        bdl[i].flags = BDL_IOC;       /* Interrupt on completion */
    }

    /* 13. Set BDL base address */
    nabm_w32(NABM_PO_BDBAR, bdl_phys);

    /* 14. Register IRQ handler */
    irq_register_handler(irq_line, ac97_irq_handler);
    irq_unmask(irq_line);

    /* 15. Get mixer fill callback from audio bridge */
    fill_callback = audio_bridge_get_fill();

    /* 16. Set Last Valid Index to wrap around all buffers */
    nabm_w8(NABM_PO_LVI, NUM_BDL - 1);

    /* 17. Start DMA: run + interrupt on completion */
    nabm_w8(NABM_PO_CR, CR_RPBM | CR_IOCE | CR_LVBIE);

    /* 18. Register with audio bridge */
    audio_bridge_register_driver(NULL, NULL);

    serial_printf("[ac97] DMA started (%d buffers x %d samples, %d KB)\n",
                  NUM_BDL, BUF_SAMPLES, (NUM_BDL * BUF_SIZE) / 1024);
    serial_print("[ac97] ready\n");
    return 0;
}

/* ── Cleanup ───────────────────────────────────────────── */

static void ac97_cleanup(void) {
    /* Stop DMA */
    nabm_w8(NABM_PO_CR, 0);
    /* Mask IRQ */
    irq_mask(irq_line);
    serial_print("[ac97] stopped\n");
}

/* ── Module Declaration ────────────────────────────────── */

static const char *deps[] = { NULL };

kaos_module_info_t kaos_module_info = {
    .magic       = KAOS_MODULE_MAGIC,
    .abi_version = KAOS_ABI_VERSION,
    .name        = "ac97",
    .version     = "1.0",
    .author      = "AIOS",
    .description = "Intel ICH AC97 audio codec driver",
    .init        = ac97_init,
    .cleanup     = ac97_cleanup,
    .dependencies = deps,
    .flags       = KAOS_FLAG_ESSENTIAL,
};
