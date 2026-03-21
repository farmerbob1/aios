#pragma once

void boot_splash_init(void);
void boot_splash_status(const char *text);
void boot_splash_module(const char *name, int loaded, int total);
void boot_splash_destroy(void);
