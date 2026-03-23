/* AIOS v2 — PCI Bus Driver (Phase 11)
 *
 * Enumerates PCI devices on bus 0 via config space ports 0xCF8/0xCFC.
 * Required for E1000 NIC and future PCI hardware drivers. */

#include "pci.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/kaos/export.h"
#include "serial.h"

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count = 0;

/* ── Config space access ─────────────────────────────── */

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)(
        (1u << 31) |                   /* enable bit */
        ((uint32_t)bus  << 16) |
        ((uint32_t)(slot & 0x1F) << 11) |
        ((uint32_t)(func & 0x07) << 8) |
        ((uint32_t)(offset & 0xFC))    /* aligned to 4 bytes */
    );
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t addr32 = offset & 0xFC;
    uint32_t old = pci_config_read32(bus, slot, func, addr32);
    int shift = (offset & 2) * 8;
    old &= ~(0xFFFF << shift);
    old |= ((uint32_t)val << shift);
    pci_config_write32(bus, slot, func, addr32, old);
}

/* ── BAR size detection ──────────────────────────────── */

static uint32_t pci_bar_size(uint8_t bus, uint8_t slot, uint8_t func, int bar_idx) {
    uint8_t offset = (uint8_t)(0x10 + bar_idx * 4);
    uint32_t original = pci_config_read32(bus, slot, func, offset);

    /* Write all 1s */
    pci_config_write32(bus, slot, func, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_config_read32(bus, slot, func, offset);

    /* Restore original */
    pci_config_write32(bus, slot, func, offset, original);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF) return 0;

    if (original & PCI_BAR_IO) {
        size_mask &= PCI_BAR_IO_MASK;
    } else {
        size_mask &= PCI_BAR_MEM_MASK;
    }

    /* Size = ~mask + 1 (invert writable bits, add 1) */
    return (~size_mask) + 1;
}

/* ── Scan one function ───────────────────────────────── */

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg0 = pci_config_read32(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t)(reg0 & 0xFFFF);
    uint16_t devid  = (uint16_t)(reg0 >> 16);

    if (vendor == 0xFFFF) return;  /* no device */
    if (device_count >= PCI_MAX_DEVICES) return;

    struct pci_device *dev = &devices[device_count];
    memset(dev, 0, sizeof(*dev));

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor;
    dev->device_id = devid;

    uint32_t reg2 = pci_config_read32(bus, slot, func, 0x08);
    dev->class_code = (uint8_t)(reg2 >> 24);
    dev->subclass   = (uint8_t)(reg2 >> 16);
    dev->prog_if    = (uint8_t)(reg2 >> 8);

    uint32_t reg3 = pci_config_read32(bus, slot, func, 0x0C);
    dev->header_type = (uint8_t)(reg3 >> 16);

    /* Read BARs (only for type 0 headers) */
    if ((dev->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            uint32_t bar = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
            dev->bar[i] = bar;
            dev->bar_is_io[i] = (bar & PCI_BAR_IO) != 0;
            dev->bar_size[i] = pci_bar_size(bus, slot, func, i);

            /* Skip next BAR if this is a 64-bit MMIO BAR */
            if (!(bar & PCI_BAR_IO) && ((bar >> 1) & 0x3) == 2) {
                i++;  /* 64-bit BAR uses two slots */
            }
        }

        /* Read IRQ info */
        uint32_t reg_3c = pci_config_read32(bus, slot, func, 0x3C);
        dev->irq_line = (uint8_t)(reg_3c & 0xFF);
        dev->irq_pin  = (uint8_t)((reg_3c >> 8) & 0xFF);
    }

    device_count++;

    serial_printf("[PCI] %02x:%02x.%x  %04x:%04x  class=%02x:%02x  IRQ=%u",
                  bus, slot, func, vendor, devid,
                  dev->class_code, dev->subclass, dev->irq_line);

    /* Print BAR0 if present */
    if (dev->bar[0]) {
        if (dev->bar_is_io[0]) {
            serial_printf("  BAR0=IO:0x%04x", dev->bar[0] & PCI_BAR_IO_MASK);
        } else {
            serial_printf("  BAR0=MEM:0x%08x (%uKB)",
                          dev->bar[0] & PCI_BAR_MEM_MASK,
                          dev->bar_size[0] / 1024);
        }
    }
    serial_print("\n");
}

/* ── Bus enumeration ─────────────────────────────────── */

init_result_t pci_init(void) {
    device_count = 0;
    serial_print("[PCI] Scanning bus 0...\n");

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t reg0 = pci_config_read32(0, slot, 0, 0x00);
        uint16_t vendor = (uint16_t)(reg0 & 0xFFFF);
        if (vendor == 0xFFFF) continue;

        pci_scan_function(0, slot, 0);

        /* Check multi-function bit */
        uint32_t reg3 = pci_config_read32(0, slot, 0, 0x0C);
        uint8_t header_type = (uint8_t)(reg3 >> 16);
        if (header_type & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                pci_scan_function(0, slot, func);
            }
        }
    }

    serial_printf("[PCI] Found %d device(s)\n", device_count);
    return INIT_OK;
}

/* ── Public API ──────────────────────────────────────── */

int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) {
            if (out) *out = devices[i];
            return i;
        }
    }
    return -1;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass) {
            if (out) *out = devices[i];
            return i;
        }
    }
    return -1;
}

void pci_enable_bus_mastering(struct pci_device *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE;
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
    serial_printf("[PCI] Bus mastering enabled for %02x:%02x.%x\n",
                  dev->bus, dev->slot, dev->func);
}

int pci_get_count(void) {
    return device_count;
}

const struct pci_device* pci_get_device(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

/* ── KAOS exports ────────────────────────────────────── */

KAOS_EXPORT(pci_find_device)
KAOS_EXPORT(pci_enable_bus_mastering)
KAOS_EXPORT(pci_config_read32)
KAOS_EXPORT(pci_config_write32)
