/* AIOS v2 — ACPI Table Parser
 *
 * Walks the ACPI table chain: RSDP → RSDT → FADT → PM1a_CNT_BLK
 * and parses DSDT AML bytecode to find the \_S5 sleep type value.
 * This is the proper, hardware-agnostic way to do ACPI shutdown. */

#include "../include/types.h"
#include "../include/acpi.h"
#include "../include/string.h"
#include "../drivers/serial.h"

/* Cached values from ACPI parsing */
static uint16_t pm1a_cnt_port = 0;
static uint16_t pm1b_cnt_port = 0;
static int      s5_slp_typ = -1;

/* ── Helpers ────────────────────────────────────────── */

static bool acpi_validate_checksum(const void *table, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static bool sig_match(const char *a, const char *b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

/* ── DSDT \_S5 Parser ──────────────────────────────── */

/* Scan DSDT AML bytecode for the \_S5_ object to extract SLP_TYPa.
 *
 * In AML, the \_S5 object is encoded as:
 *   08 5F 53 35 5F    NameOp "_S5_"
 *   12 ...             PackageOp
 *   <pkglen> <numelem>
 *   <first element>    = SLP_TYPa
 *
 * The first element can be:
 *   0x0A <byte>        BytePrefix + value
 *   0x0B <word>        WordPrefix + value
 *   0x00 - 0x3F        direct byte (ZeroOp=0x00, OneOp=0x01, etc.)
 */
static int parse_s5_from_dsdt(const struct acpi_sdt_header *dsdt) {
    const uint8_t *aml = (const uint8_t *)dsdt;
    uint32_t len = dsdt->length;

    /* Search for "_S5_" in the AML stream */
    for (uint32_t i = 0; i + 4 < len; i++) {
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            /* Found \_S5_ — check if preceded by NameOp (0x08) */
            if (i > 0 && aml[i-1] != 0x08) {
                continue;  /* Not a Name declaration, keep searching */
            }

            /* Skip past "_S5_" to the PackageOp */
            uint32_t j = i + 4;
            if (j >= len || aml[j] != 0x12) {
                serial_print("[acpi] _S5_ found but no PackageOp follows\n");
                continue;
            }
            j++;  /* skip PackageOp */

            /* Parse PkgLength (1-4 bytes) */
            if (j >= len) return -1;
            uint8_t lead = aml[j];
            uint32_t pkg_len_bytes = ((lead >> 6) & 0x03);
            if (pkg_len_bytes == 0) {
                j += 1;  /* single-byte PkgLength */
            } else {
                j += 1 + pkg_len_bytes;  /* multi-byte PkgLength */
            }

            if (j >= len) return -1;

            /* Skip NumElements byte */
            j++;

            /* Now read the first element (SLP_TYPa) */
            if (j >= len) return -1;

            int slp_typ;
            if (aml[j] == 0x0A) {
                /* BytePrefix */
                j++;
                if (j >= len) return -1;
                slp_typ = aml[j];
            } else if (aml[j] == 0x0B) {
                /* WordPrefix */
                j++;
                if (j + 1 >= len) return -1;
                slp_typ = aml[j] | (aml[j+1] << 8);
            } else if (aml[j] == 0x00) {
                slp_typ = 0;  /* ZeroOp */
            } else if (aml[j] == 0x01) {
                slp_typ = 1;  /* OneOp */
            } else if (aml[j] <= 0x3F) {
                slp_typ = aml[j];  /* Direct byte constant */
            } else {
                serial_print("[acpi] _S5_ unexpected encoding for SLP_TYPa\n");
                return -1;
            }

            serial_printf("[acpi] _S5_ SLP_TYPa = %d\n", slp_typ);
            return slp_typ;
        }
    }

    serial_print("[acpi] _S5_ not found in DSDT\n");
    return -1;
}

/* ── Main Init ──────────────────────────────────────── */

init_result_t acpi_init(struct boot_info *info) {
    if (!info->acpi_rsdp) {
        serial_print("[acpi] No RSDP address from bootloader\n");
        return INIT_WARN;
    }

    /* ── Validate RSDP ─────────────────────────── */
    const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)(uint32_t)info->acpi_rsdp;

    if (!sig_match(rsdp->signature, "RSD PTR ", 8)) {
        serial_print("[acpi] Invalid RSDP signature\n");
        return INIT_WARN;
    }

    if (!acpi_validate_checksum(rsdp, 20)) {
        serial_print("[acpi] RSDP checksum failed\n");
        return INIT_WARN;
    }

    serial_printf("[acpi] RSDP at 0x%x, revision %d, OEM %.6s\n",
                  info->acpi_rsdp, rsdp->revision, rsdp->oem_id);

    /* ── Find RSDT ─────────────────────────────── */
    const struct acpi_sdt_header *rsdt =
        (const struct acpi_sdt_header *)(uint32_t)rsdp->rsdt_address;

    if (!sig_match(rsdt->signature, "RSDT", 4)) {
        serial_print("[acpi] Invalid RSDT signature\n");
        return INIT_WARN;
    }

    serial_printf("[acpi] RSDT at 0x%x, length %u\n",
                  rsdp->rsdt_address, rsdt->length);

    /* ── Scan RSDT entries for FADT ────────────── */
    uint32_t entry_count = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
    const uint32_t *entries = (const uint32_t *)((const uint8_t *)rsdt + sizeof(struct acpi_sdt_header));

    const struct acpi_fadt *fadt = 0;
    const struct acpi_sdt_header *dsdt_header = 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uint32_t)entries[i];

        if (sig_match(hdr->signature, "FACP", 4)) {
            fadt = (const struct acpi_fadt *)hdr;
            serial_printf("[acpi] FADT at 0x%x\n", entries[i]);
        }
    }

    if (!fadt) {
        serial_print("[acpi] FADT not found in RSDT\n");
        return INIT_WARN;
    }

    /* ── Extract PM1a/PM1b control block ports ─── */
    pm1a_cnt_port = (uint16_t)fadt->pm1a_control_block;
    pm1b_cnt_port = (uint16_t)fadt->pm1b_control_block;

    serial_printf("[acpi] PM1a_CNT_BLK = 0x%x, PM1b_CNT_BLK = 0x%x\n",
                  pm1a_cnt_port, pm1b_cnt_port);

    /* ── Parse DSDT for \_S5 sleep type ────────── */
    if (fadt->dsdt) {
        dsdt_header = (const struct acpi_sdt_header *)(uint32_t)fadt->dsdt;
        if (sig_match(dsdt_header->signature, "DSDT", 4)) {
            serial_printf("[acpi] DSDT at 0x%x, length %u\n",
                          fadt->dsdt, dsdt_header->length);
            s5_slp_typ = parse_s5_from_dsdt(dsdt_header);
        } else {
            serial_print("[acpi] Invalid DSDT signature\n");
        }
    }

    if (pm1a_cnt_port && s5_slp_typ >= 0) {
        serial_printf("[acpi] Ready: shutdown via port 0x%x, SLP_TYP=%d\n",
                      pm1a_cnt_port, s5_slp_typ);
        return INIT_OK;
    }

    return INIT_WARN;
}

uint16_t acpi_get_pm1a_cnt(void) { return pm1a_cnt_port; }
uint16_t acpi_get_pm1b_cnt(void) { return pm1b_cnt_port; }
int      acpi_get_s5_slp_typ(void) { return s5_slp_typ; }
