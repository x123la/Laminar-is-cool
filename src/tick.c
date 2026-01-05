#include "tick.h"
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static struct timespec next_ts;
static int initialized = 0;

static void panic(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static inline void timespec_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

void tick_init_1khz(void) {
    if (clock_gettime(CLOCK_MONOTONIC, &next_ts) == -1) {
        panic("tick_init: clock_gettime failed");
    }
    // Snap to next millisecond edge for clean alignment
    long current_ns = next_ts.tv_nsec;
    long remainder = current_ns % 1000000L;
    long wait_ns = 1000000L - remainder;
    timespec_add_ns(&next_ts, wait_ns);
    initialized = 1;
}

void tick_wait_next(void) {
    if (!initialized) tick_init_1khz();

    // Advance target by 1ms
    timespec_add_ns(&next_ts, 1000000L);

    while (1) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_ts, NULL);
        if (rc == 0) {
            break;
        } else if (rc == EINTR) {
            // Interrupted by signal, retry with same absolute deadline
            continue;
        } else {
            // EINVAL or EOPNOTSUPP - fatal
            fprintf(stderr, "clock_nanosleep failed: %d\n", rc);
            exit(EXIT_FAILURE);
        }
    }
}
