#ifndef EWRTC_PAL_H
#define EWRTC_PAL_H
#include "pal/memory.h"
#include "pal/clock.h"
#include "pal/random.h"
#include "pal/log.h"
#include "pal/threads.h"
#include "pal/network.h"
#include "pal/events.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Each group has its own context; replacing one service never changes another.
 * Tables are copied by constructors. Contexts must outlive their objects.
 * Allocators must be thread-safe when used with Session/libjuice. */
typedef struct ewrtc_pal {
    ewrtc_allocator memory;
    ewrtc_clock clock;
    ewrtc_random random;
    ewrtc_threads threads;
    ewrtc_network network;
    ewrtc_logger log;
    ewrtc_events events;
} ewrtc_pal;
enum {
    EWRTC_PAL_ALLOCATOR = 1u << 0, /* allocate + release */
    EWRTC_PAL_RESIZE = 1u << 1,
    EWRTC_PAL_MONOTONIC = 1u << 2,
    EWRTC_PAL_UTC = 1u << 3,
    EWRTC_PAL_RANDOM = 1u << 4,
    EWRTC_PAL_MUTEX = 1u << 5,
    EWRTC_PAL_THREAD = 1u << 6,
    EWRTC_PAL_CONDITION = 1u << 7,
    EWRTC_PAL_UDP_SEND = 1u << 8,
    EWRTC_PAL_UDP_SOCKET = 1u << 9, /* open + close + local + receive */
    EWRTC_PAL_DNS = 1u << 10,
    EWRTC_PAL_INTERFACES = 1u << 11,
    EWRTC_PAL_EVENTS = 1u << 12,
    EWRTC_PAL_MEMORY = EWRTC_PAL_ALLOCATOR | EWRTC_PAL_RESIZE,
    EWRTC_PAL_CLOCK = EWRTC_PAL_MONOTONIC | EWRTC_PAL_UTC,
    EWRTC_PAL_THREADS = EWRTC_PAL_MUTEX | EWRTC_PAL_THREAD | EWRTC_PAL_CONDITION,
    EWRTC_PAL_NETWORK = EWRTC_PAL_UDP_SEND | EWRTC_PAL_UDP_SOCKET | EWRTC_PAL_DNS | EWRTC_PAL_INTERFACES
};
/* Components require explicit services; no implicit platform defaults. */
int ewrtc_pal_validate(const ewrtc_pal *, unsigned capabilities);

#ifdef __cplusplus
}
#endif
#endif
