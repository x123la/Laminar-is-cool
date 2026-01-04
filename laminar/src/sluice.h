#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes NFQUEUE and starts the internal poll thread.
// queue_num is the NFQUEUE number (0 by default).
int sluice_init(uint16_t queue_num);

// Sensors
int64_t sluice_get_pressure_bytes(void);
int64_t sluice_get_inflow_bytes_and_reset(void);

// Actuator command for NEXT tick: bytes to release.
void sluice_set_release_budget_bytes(int64_t bytes);

// Clean shutdown: accept everything remaining, stop thread, close queue.
void sluice_shutdown(void);

#ifdef __cplusplus
}
#endif
