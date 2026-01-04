#include "tick.h"

#include <time.h>
#include <stdint.h>

static int64_t next_ns = 0;

static inline int64_t now_ns(void) {
struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);
return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void tick_init_1khz(void) {
int64_t n = now_ns();
// Next tick aligned to the next millisecond boundary
next_ns = (n / 1000000LL + 1) * 1000000LL;
}

void tick_wait_next(void) {
if (next_ns == 0) tick_init_1khz();

struct timespec ts;
ts.tv_sec = (time_t)(next_ns / 1000000000LL);
ts.tv_nsec = (long)(next_ns % 1000000000LL);

// Absolute sleep to avoid drift
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

int64_t actual_now = now_ns();
if (actual_now > next_ns + 2000000LL) { // > 2ms late
    fprintf(stderr, "WARNING: Laminar loop drift detected (%ld us late)\n", 
            (long)((actual_now - next_ns) / 1000));
}

next_ns += 1000000LL; // +1ms
}
