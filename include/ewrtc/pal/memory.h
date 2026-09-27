#ifndef EWRTC_PAL_MEMORY_H
#define EWRTC_PAL_MEMORY_H
#include "../common.h"
/* Standard C alignment. resize failure preserves the original allocation.
 * Services must be thread-safe for Session or libjuice; contexts are borrowed. */
typedef struct {
    void *ctx;
    void *(*allocate)(void *, size_t);
    void *(*resize)(void *, void *, size_t);
    void (*release)(void *, void *);
} ewrtc_allocator;
#endif
