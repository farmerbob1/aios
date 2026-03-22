/* kernel/kaos/kaos_io_wrappers.c — Non-inline wrappers for port I/O
 *
 * The inline functions in include/io.h have no linkable symbol.
 * These wrappers provide linkable versions for KAOS module use. */

#include "../../include/io.h"
#include "../../include/kaos/export.h"

uint8_t kaos_inb(uint16_t port) { return inb(port); }
void kaos_outb(uint16_t port, uint8_t val) { outb(port, val); }
uint16_t kaos_inw(uint16_t port) { return inw(port); }
void kaos_outw(uint16_t port, uint16_t val) { outw(port, val); }
uint32_t kaos_inl(uint16_t port) { return inl(port); }
void kaos_outl(uint16_t port, uint32_t val) { outl(port, val); }
void kaos_io_wait(void) { io_wait(); }

KAOS_EXPORT(kaos_inb)
KAOS_EXPORT(kaos_outb)
KAOS_EXPORT(kaos_inw)
KAOS_EXPORT(kaos_outw)
KAOS_EXPORT(kaos_inl)
KAOS_EXPORT(kaos_outl)
KAOS_EXPORT(kaos_io_wait)
