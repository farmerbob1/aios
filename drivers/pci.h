/* AIOS v2 — PCI Bus Driver (Phase 11) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define PCI_MAX_DEVICES 32

/* PCI config space ports */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

/* PCI command register bits */
#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEM_SPACE       (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)
#define PCI_CMD_INT_DISABLE     (1 << 10)

/* BAR type detection */
#define PCI_BAR_IO              (1 << 0)
#define PCI_BAR_MEM_MASK        0xFFFFFFF0
#define PCI_BAR_IO_MASK         0xFFFFFFFC

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

init_result_t pci_init(void);

/* Config space access */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

/* Device queries */
int  pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out);
int  pci_find_class(uint8_t class_code, uint8_t subclass, struct pci_device *out);
void pci_enable_bus_mastering(struct pci_device *dev);
int  pci_get_count(void);
const struct pci_device* pci_get_device(int index);
