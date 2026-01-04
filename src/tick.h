#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes a 1000 Hz tick schedule based on CLOCK_MONOTONIC.
void tick_init_1khz(void);

// Sleeps until the next tick boundary (absolute schedule, no drift).
void tick_wait_next(void);

#ifdef __cplusplus
}
#endif
