/* AIOS v2 — Intel E1000 NIC Driver (Phase 11)
 *
 * KAOS module for the Intel 82540EM (QEMU default NIC).
 * PCI vendor 0x8086, device 0x100E.
 *
 * Uses MMIO register access via BAR0, DMA descriptor rings for TX/RX,
 * and PIC IRQ for interrupt-driven packet reception. */

#include <kaos/module.h>
#include <types.h>

/* ── Kernel API (resolved via KAOS symbol table) ────────── */

extern void  serial_printf(const char *fmt, ...);
extern void  serial_print(const char *str);
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern void *kzmalloc(size_t size);
extern void  memset(void *dst, int c, size_t n);
extern void  memcpy(void *dst, const void *src, size_t n);

extern int   pci_find_device(uint16_t vendor, uint16_t device, void *out);
extern void  pci_enable_bus_mastering(void *dev);
extern uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

extern uint32_t pmm_alloc_pages(uint32_t count);
extern void     pmm_free_pages(uint32_t addr, uint32_t count);
extern void     vmm_map_range(uint32_t virt, uint32_t phys, uint32_t size, uint32_t flags);

extern void  irq_register_handler(int irq, void (*handler)(void));
extern void  irq_unmask(int irq);
extern void  irq_mask(int irq);

extern int   netif_bridge_register_driver(void *tx_fn, void *get_mac_fn);
extern void  netif_bridge_rx(void *buf);
extern void *netbuf_alloc(void);
extern void  netbuf_free(void *buf);

/* ── PCI device struct (must match drivers/pci.h exactly) ── */

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

/* ── Page table flags (match kernel/vmm.h) ──────────────── */
#define PTE_PRESENT     0x001
#define PTE_WRITABLE    0x002
#define PTE_NOCACHE     0x010
#define PTE_WRITETHROUGH 0x008
#define PAGE_SIZE       4096

/* ── E1000 Register Offsets ─────────────────────────────── */

#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */
#define E1000_EECD      0x0010  /* EEPROM/Flash Control */
#define E1000_EERD      0x0014  /* EEPROM Read */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100  /* Receive Control */
#define E1000_TCTL      0x0400  /* Transmit Control */
#define E1000_TIPG      0x0410  /* TX Inter-Packet Gap */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Low */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Ring Length */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Low */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Ring Length */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */
#define E1000_RAL       0x5400  /* Receive Address Low */
#define E1000_RAH       0x5404  /* Receive Address High */
#define E1000_MTA       0x5200  /* Multicast Table Array (128 entries) */

/* CTRL bits */
#define E1000_CTRL_SLU      (1 << 6)   /* Set Link Up */
#define E1000_CTRL_RST      (1 << 26)  /* Device Reset */

/* STATUS bits */
#define E1000_STATUS_LU     (1 << 1)   /* Link Up */

/* RCTL bits */
#define E1000_RCTL_EN       (1 << 1)   /* Receiver Enable */
#define E1000_RCTL_SBP      (1 << 2)   /* Store Bad Packets */
#define E1000_RCTL_UPE      (1 << 3)   /* Unicast Promiscuous */
#define E1000_RCTL_MPE      (1 << 4)   /* Multicast Promiscuous */
#define E1000_RCTL_BAM      (1 << 15)  /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_2K (0 << 16)  /* Buffer Size 2048 */
#define E1000_RCTL_SECRC    (1 << 26)  /* Strip Ethernet CRC */

/* TCTL bits */
#define E1000_TCTL_EN       (1 << 1)   /* Transmitter Enable */
#define E1000_TCTL_PSP      (1 << 3)   /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT 4          /* Collision Threshold */
#define E1000_TCTL_COLD_SHIFT 12       /* Collision Distance */

/* Interrupt bits */
#define E1000_ICR_TXDW      (1 << 0)   /* TX Descriptor Written Back */
#define E1000_ICR_TXQE      (1 << 1)   /* TX Queue Empty */
#define E1000_ICR_LSC       (1 << 2)   /* Link Status Change */
#define E1000_ICR_RXDMT0    (1 << 4)   /* RX Descriptor Minimum Threshold */
#define E1000_ICR_RXO       (1 << 6)   /* Receiver Overrun */
#define E1000_ICR_RXT0      (1 << 7)   /* Receiver Timer Interrupt */

/* TX descriptor command bits */
#define E1000_TXD_CMD_EOP   (1 << 0)   /* End of Packet */
#define E1000_TXD_CMD_IFCS  (1 << 1)   /* Insert FCS/CRC */
#define E1000_TXD_CMD_RS    (1 << 3)   /* Report Status */

/* TX descriptor status bits */
#define E1000_TXD_STAT_DD   (1 << 0)   /* Descriptor Done */

/* RX descriptor status bits */
#define E1000_RXD_STAT_DD   (1 << 0)   /* Descriptor Done */
#define E1000_RXD_STAT_EOP  (1 << 1)   /* End of Packet */

/* ── Descriptor Structures (must match hardware layout) ─── */

#define NUM_TX_DESC  32
#define NUM_RX_DESC  32
#define RX_BUF_SIZE  2048

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* ── Module State (globals — IRQ handler needs access) ──── */

static volatile uint32_t *mmio_base = NULL;
static uint32_t mmio_phys = 0;
static uint32_t mmio_size = 0;
static uint8_t  mac_addr[6];
static uint8_t  irq_line = 0;

/* Descriptor rings (physically contiguous for DMA) */
static struct e1000_tx_desc *tx_descs = NULL;
static struct e1000_rx_desc *rx_descs = NULL;
static uint32_t tx_ring_phys = 0;
static uint32_t rx_ring_phys = 0;

/* TX/RX buffers */
static uint8_t *tx_buffers = NULL;
static uint8_t *rx_buffers = NULL;
static uint32_t tx_buf_phys = 0;
static uint32_t rx_buf_phys = 0;

/* Ring indices */
static uint32_t tx_tail = 0;
static uint32_t rx_tail = 0;

/* PCI device info (stored for cleanup) */
static uint8_t pci_bus, pci_slot, pci_func;

/* Stats */
static uint32_t rx_count = 0;
static uint32_t tx_count = 0;

/* ── MMIO Helpers ───────────────────────────────────────── */

static inline void e1000_write(uint32_t reg, uint32_t val) {
    mmio_base[reg / 4] = val;
}

static inline uint32_t e1000_read(uint32_t reg) {
    return mmio_base[reg / 4];
}

/* ── EEPROM Read ────────────────────────────────────────── */

static uint16_t e1000_eeprom_read(uint8_t addr) {
    e1000_write(E1000_EERD, (1) | ((uint32_t)addr << 8));
    uint32_t val;
    for (int i = 0; i < 1000; i++) {
        val = e1000_read(E1000_EERD);
        if (val & (1 << 4)) {  /* Done bit */
            return (uint16_t)(val >> 16);
        }
    }
    return 0;
}

/* ── Read MAC Address ───────────────────────────────────── */

static void e1000_read_mac(void) {
    /* Try RAL/RAH first (QEMU usually sets these) */
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);

    if (ral != 0 || (rah & 0xFFFF) != 0) {
        mac_addr[0] = (uint8_t)(ral);
        mac_addr[1] = (uint8_t)(ral >> 8);
        mac_addr[2] = (uint8_t)(ral >> 16);
        mac_addr[3] = (uint8_t)(ral >> 24);
        mac_addr[4] = (uint8_t)(rah);
        mac_addr[5] = (uint8_t)(rah >> 8);
    } else {
        /* Fallback: read from EEPROM */
        uint16_t w0 = e1000_eeprom_read(0);
        uint16_t w1 = e1000_eeprom_read(1);
        uint16_t w2 = e1000_eeprom_read(2);
        mac_addr[0] = (uint8_t)(w0);
        mac_addr[1] = (uint8_t)(w0 >> 8);
        mac_addr[2] = (uint8_t)(w1);
        mac_addr[3] = (uint8_t)(w1 >> 8);
        mac_addr[4] = (uint8_t)(w2);
        mac_addr[5] = (uint8_t)(w2 >> 8);
    }

    /* Program MAC into RAL/RAH (ensure NIC uses it) */
    e1000_write(E1000_RAL,
        (uint32_t)mac_addr[0] | ((uint32_t)mac_addr[1] << 8) |
        ((uint32_t)mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24));
    e1000_write(E1000_RAH,
        (uint32_t)mac_addr[4] | ((uint32_t)mac_addr[5] << 8) |
        (1u << 31));  /* Address Valid bit */
}

/* ── Allocate DMA Memory ────────────────────────────────── */

static uint32_t alloc_dma(uint32_t pages) {
    uint32_t phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    /* Identity-map with NOCACHE for DMA coherency */
    vmm_map_range(phys, phys, pages * PAGE_SIZE,
                  PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    memset((void *)phys, 0, pages * PAGE_SIZE);
    return phys;
}

/* ── Setup TX Ring ──────────────────────────────────────── */

static int e1000_tx_init(void) {
    /* Allocate descriptor ring (512 bytes = 1 page) */
    tx_ring_phys = alloc_dma(1);
    if (!tx_ring_phys) return -1;
    tx_descs = (struct e1000_tx_desc *)tx_ring_phys;

    /* Allocate TX buffers: NUM_TX_DESC * 2048 = 64KB = 16 pages */
    uint32_t buf_pages = (NUM_TX_DESC * RX_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    tx_buf_phys = alloc_dma(buf_pages);
    if (!tx_buf_phys) return -1;
    tx_buffers = (uint8_t *)tx_buf_phys;

    /* Initialize descriptors */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = (uint64_t)(tx_buf_phys + i * RX_BUF_SIZE);
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TXD_STAT_DD;  /* Mark as done (available) */
    }

    /* Program TX registers */
    e1000_write(E1000_TDBAL, tx_ring_phys);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_tail = 0;

    /* TX control: enable, pad short packets, collision params */
    e1000_write(E1000_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP |
        (15 << E1000_TCTL_CT_SHIFT) |
        (64 << E1000_TCTL_COLD_SHIFT));

    /* TX inter-packet gap (recommended values for IEEE 802.3) */
    e1000_write(E1000_TIPG, (10) | (10 << 10) | (10 << 20));

    serial_printf("[e1000] TX ring: %d descriptors at 0x%08x\n",
                  NUM_TX_DESC, tx_ring_phys);
    return 0;
}

/* ── Setup RX Ring ──────────────────────────────────────── */

static int e1000_rx_init(void) {
    /* Allocate descriptor ring (512 bytes = 1 page) */
    rx_ring_phys = alloc_dma(1);
    if (!rx_ring_phys) return -1;
    rx_descs = (struct e1000_rx_desc *)rx_ring_phys;

    /* Allocate RX buffers: NUM_RX_DESC * 2048 = 64KB = 16 pages */
    uint32_t buf_pages = (NUM_RX_DESC * RX_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    rx_buf_phys = alloc_dma(buf_pages);
    if (!rx_buf_phys) return -1;
    rx_buffers = (uint8_t *)rx_buf_phys;

    /* Initialize descriptors with buffer addresses */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(rx_buf_phys + i * RX_BUF_SIZE);
        rx_descs[i].status = 0;
    }

    /* Program RX registers */
    e1000_write(E1000_RDBAL, rx_ring_phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NUM_RX_DESC - 1);
    rx_tail = NUM_RX_DESC - 1;

    /* Clear multicast table */
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + i * 4, 0);
    }

    /* RX control: enable, accept broadcast, 2K buffers, strip CRC */
    e1000_write(E1000_RCTL,
        E1000_RCTL_EN | E1000_RCTL_BAM |
        E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC);

    serial_printf("[e1000] RX ring: %d descriptors at 0x%08x\n",
                  NUM_RX_DESC, rx_ring_phys);
    return 0;
}

/* ── Transmit ───────────────────────────────────────────── */

static int e1000_transmit(const uint8_t *data, uint16_t len) {
    if (len > 1500) return -1;

    uint32_t idx = tx_tail;
    struct e1000_tx_desc *desc = &tx_descs[idx];

    /* Wait for descriptor to be available */
    if (!(desc->status & E1000_TXD_STAT_DD)) {
        serial_print("[e1000] TX ring full\n");
        return -1;
    }

    /* Copy data to TX buffer */
    memcpy((void *)(tx_buf_phys + idx * RX_BUF_SIZE), data, len);

    /* Set up descriptor */
    desc->addr = (uint64_t)(tx_buf_phys + idx * RX_BUF_SIZE);
    desc->length = len;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;

    /* Advance tail */
    tx_tail = (idx + 1) % NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_tail);
    tx_count++;

    return 0;
}

/* ── Receive (called from IRQ handler) ──────────────────── */

static void e1000_receive(void) {
    while (1) {
        uint32_t idx = (rx_tail + 1) % NUM_RX_DESC;
        struct e1000_rx_desc *desc = &rx_descs[idx];

        if (!(desc->status & E1000_RXD_STAT_DD)) break;  /* No more packets */

        uint16_t len = desc->length;
        if ((desc->status & E1000_RXD_STAT_EOP) && len > 0 && len <= 1536) {
            /* Allocate a netbuf and copy packet data */
            /* netbuf layout: data[1536], len(u16), flags(u16), next(ptr) */
            uint8_t *nbuf = (uint8_t *)netbuf_alloc();
            if (nbuf) {
                memcpy(nbuf, (void *)(rx_buf_phys + idx * RX_BUF_SIZE), len);
                /* Set len field (at offset 1536 in struct netbuf) */
                *(uint16_t *)(nbuf + 1536) = len;
                netif_bridge_rx(nbuf);
                rx_count++;
            }
        }

        /* Reset descriptor for reuse */
        desc->status = 0;
        rx_tail = idx;
        e1000_write(E1000_RDT, rx_tail);
    }
}

/* ── IRQ Handler ────────────────────────────────────────── */

static void e1000_irq_handler(void) {
    uint32_t icr = e1000_read(E1000_ICR);  /* Reading ICR clears it */

    if (icr & E1000_ICR_RXT0) {
        e1000_receive();
    }

    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_read(E1000_STATUS);
        serial_printf("[e1000] link %s\n",
                      (status & E1000_STATUS_LU) ? "UP" : "DOWN");
    }
}

/* ── Get MAC (for netif_bridge) ─────────────────────────── */

static void e1000_get_mac(uint8_t out[6]) {
    memcpy(out, mac_addr, 6);
}

/* ── Module Init ────────────────────────────────────────── */

static int e1000_init(void) {
    serial_print("[e1000] Intel 82540EM NIC driver\n");

    /* --- PCI device detection --- */
    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));

    if (pci_find_device(0x8086, 0x100E, &dev) < 0) {
        serial_print("[e1000] ERROR: device not found\n");
        return -1;
    }

    pci_bus  = dev.bus;
    pci_slot = dev.slot;
    pci_func = dev.func;
    irq_line = dev.irq_line;
    mmio_size = dev.bar_size[0];

    if (dev.bar_is_io[0] || dev.bar[0] == 0) {
        serial_print("[e1000] ERROR: BAR0 is not MMIO\n");
        return -1;
    }

    mmio_phys = dev.bar[0] & 0xFFFFFFF0;  /* Mask type bits */

    serial_printf("[e1000] PCI %02x:%02x.%x  IRQ=%u  BAR0=0x%08x (%u KB)\n",
                  pci_bus, pci_slot, pci_func, irq_line,
                  mmio_phys, mmio_size / 1024);

    /* --- Enable bus mastering for DMA --- */
    pci_enable_bus_mastering(&dev);

    /* --- Map MMIO region --- */
    uint32_t mmio_pages = (mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;
    vmm_map_range(mmio_phys, mmio_phys, mmio_pages * PAGE_SIZE,
                  PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
    mmio_base = (volatile uint32_t *)mmio_phys;

    serial_printf("[e1000] MMIO mapped: 0x%08x (%u pages)\n",
                  mmio_phys, mmio_pages);

    /* --- Reset device --- */
    e1000_write(E1000_IMC, 0xFFFFFFFF);  /* Disable all interrupts */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);

    /* Brief delay for reset (read STATUS a few times) */
    for (volatile int i = 0; i < 100000; i++) {
        (void)e1000_read(E1000_STATUS);
    }

    e1000_write(E1000_IMC, 0xFFFFFFFF);  /* Disable interrupts again after reset */

    /* --- Read MAC address --- */
    e1000_read_mac();
    serial_printf("[e1000] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  mac_addr[0], mac_addr[1], mac_addr[2],
                  mac_addr[3], mac_addr[4], mac_addr[5]);

    /* --- Set link up --- */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    ctrl &= ~(1u << 3);   /* Clear LRST */
    ctrl &= ~(1u << 31);  /* Clear PHY_RST */
    e1000_write(E1000_CTRL, ctrl);

    /* --- Initialize TX and RX --- */
    if (e1000_tx_init() < 0) {
        serial_print("[e1000] ERROR: TX init failed\n");
        return -1;
    }
    if (e1000_rx_init() < 0) {
        serial_print("[e1000] ERROR: RX init failed\n");
        return -1;
    }

    /* --- Register IRQ handler --- */
    irq_register_handler(irq_line, e1000_irq_handler);
    irq_unmask(irq_line);

    /* Enable receive interrupts */
    e1000_write(E1000_IMS,
        E1000_ICR_RXT0 | E1000_ICR_LSC | E1000_ICR_RXO | E1000_ICR_RXDMT0);

    /* --- Check link status --- */
    uint32_t status = e1000_read(E1000_STATUS);
    serial_printf("[e1000] link: %s\n",
                  (status & E1000_STATUS_LU) ? "UP" : "DOWN");

    /* --- Register with netif bridge --- */
    netif_bridge_register_driver((void *)e1000_transmit, (void *)e1000_get_mac);

    serial_printf("[e1000] ready (TX:%d RX:%d descriptors)\n",
                  NUM_TX_DESC, NUM_RX_DESC);
    return 0;
}

/* ── Module Cleanup ─────────────────────────────────────── */

static void e1000_cleanup(void) {
    /* Disable interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    irq_mask(irq_line);

    /* Disable TX and RX */
    e1000_write(E1000_TCTL, 0);
    e1000_write(E1000_RCTL, 0);

    serial_printf("[e1000] unloaded (TX:%u RX:%u packets)\n", tx_count, rx_count);
}

/* ── Module Declaration ─────────────────────────────────── */

static const char *deps[] = { NULL };

kaos_module_info_t kaos_module_info = {
    .magic       = KAOS_MODULE_MAGIC,
    .abi_version = KAOS_ABI_VERSION,
    .name        = "e1000",
    .version     = "1.0",
    .author      = "AIOS",
    .description = "Intel 82540EM NIC driver",
    .init        = e1000_init,
    .cleanup     = e1000_cleanup,
    .dependencies = deps,
    .flags       = KAOS_FLAG_ESSENTIAL,
};
