#ifndef EWRTC_PAL_RANDOM_H
#define EWRTC_PAL_RANDOM_H
#include "../common.h"
/* Cryptographic random bytes: fill the entire buffer or return an error. */
typedef struct {
    void *ctx;
    int (*bytes)(void *, void *, size_t);
} ewrtc_random;
#endif
