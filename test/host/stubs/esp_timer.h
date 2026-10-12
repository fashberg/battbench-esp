#pragma once
#include <stdint.h>
extern int64_t fake_us;
static inline int64_t esp_timer_get_time(void) { return fake_us; }
