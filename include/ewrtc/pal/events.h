#ifndef EWRTC_PAL_EVENTS_H
#define EWRTC_PAL_EVENTS_H
#include "network.h"
typedef void *ewrtc_waiter;
/* Owner serializes add/remove/wait; wake is thread-safe and sticky. Tokens are
 * nonzero generations, never object pointers. UINT32_MAX means infinite wait.
 * Socket handles must belong to the matching network provider. */
typedef struct {
    void *ctx;
    int (*create)(void *, ewrtc_waiter *);
    void (*destroy)(void *, ewrtc_waiter);
    int (*add)(void *, ewrtc_waiter, ewrtc_socket, uint64_t token);
    int (*remove)(void *, ewrtc_waiter, ewrtc_socket);
    int (*wait)(void *, ewrtc_waiter, uint64_t *tokens, size_t capacity,
                size_t *count, uint32_t timeout_ms);
    int (*wake)(void *, ewrtc_waiter);
} ewrtc_events;
#endif
