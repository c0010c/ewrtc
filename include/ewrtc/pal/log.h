#ifndef EWRTC_PAL_LOG_H
#define EWRTC_PAL_LOG_H
#include "../common.h"
/* Increasing severity; preserves the existing error level 3. */
typedef enum {
    EWRTC_LOG_DEBUG = 0, EWRTC_LOG_INFO = 1, EWRTC_LOG_WARN = 2, EWRTC_LOG_ERROR = 3
} ewrtc_log_level;
/* Optional synchronous sink. Receives a complete, single-line record without a
 * trailing newline. May run concurrently on API, preparation and worker threads.
 * Must be thread-safe, return promptly, and not call back into the SDK.
 * Text is borrowed for this call only. NULL write disables logging. */
typedef struct {
    void *ctx;
    void (*write)(void *, int level, const char *);
} ewrtc_logger;
#endif
