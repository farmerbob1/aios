/* AIOS v2 — Boot Display (honest status logging) */

#pragma once

#include "../include/boot_info.h"

void boot_log(const char* component, init_result_t result);
void boot_log_detail(const char* component, init_result_t result, const char* detail);
void boot_print(const char* msg);
void boot_display_banner(void);
