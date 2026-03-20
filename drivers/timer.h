/* AIOS v2 — PIT Timer Driver (Phase 2) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define PIT_FREQUENCY    250   /* Hz — design contract */
#define PIT_BASE_FREQ    1193182

init_result_t timer_init(void);
void     timer_handler(void);
uint64_t timer_get_ticks(void);
uint32_t timer_get_uptime_seconds(void);
uint32_t timer_get_frequency(void);
void     timer_wait(uint32_t ms);
