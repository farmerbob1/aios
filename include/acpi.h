/* AIOS v2 — ACPI Table Parser
 *
 * Walks RSDP → RSDT → FADT to find PM1a_CNT_BLK,
 * then parses DSDT AML to find \_S5 sleep type. */

#pragma once

#include "types.h"
#include "boot_info.h"

/* ── ACPI Table Structures ──────────────────────────── */

struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* FADT (Fixed ACPI Description Table) — we only need a few fields */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;   /* THIS is what we need for shutdown */
    uint32_t pm1b_control_block;
    /* ... more fields follow but we don't need them */
} __attribute__((packed));

/* ── API ────────────────────────────────────────────── */

/* Parse ACPI tables from boot_info RSDP pointer.
 * Returns INIT_OK if PM1a_CNT and S5 type found, INIT_WARN otherwise. */
init_result_t acpi_init(struct boot_info *info);

/* Get PM1a control block I/O port (0 if not found) */
uint16_t acpi_get_pm1a_cnt(void);

/* Get PM1b control block I/O port (0 if not found) */
uint16_t acpi_get_pm1b_cnt(void);

/* Get SLP_TYP value for S5 (soft-off) state (-1 if not found) */
int acpi_get_s5_slp_typ(void);
