#ifndef EWRTC_PAL_LOG_H
#define EWRTC_PAL_LOG_H
#include "../common.h"
typedef struct {
    void *ctx;
    void (*write)(void *, int level, const char *);
} ewrtc_logger;
#endif
