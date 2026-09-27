#ifndef EWRTC_PAL_CLOCK_H
#define EWRTC_PAL_CLOCK_H
#include "../common.h"
/* Monotonic milliseconds for intervals; Unix UTC microseconds for dates. */
typedef struct {
    void *ctx;
    uint64_t (*monotonic_ms)(void *);
    uint64_t (*utc_us)(void *);
} ewrtc_clock;
#endif
