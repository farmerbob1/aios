/* AIOS v2 — Power Management */

#pragma once

/* Flush caches, disable interrupts, power off via ACPI (QEMU PIIX4 port 0x604).
 * Falls back to cli;hlt if ACPI shutdown fails. Does not return. */
void system_shutdown(void);

/* Flush caches, disable interrupts, reset CPU via keyboard controller (port 0x64).
 * Falls back to triple-fault if keyboard reset fails. Does not return. */
void system_restart(void);
