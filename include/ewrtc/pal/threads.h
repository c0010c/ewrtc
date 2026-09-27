#ifndef EWRTC_PAL_THREADS_H
#define EWRTC_PAL_THREADS_H
#include "../common.h"
typedef void *ewrtc_thread;
typedef void *ewrtc_mutex;
typedef void *ewrtc_condition;
typedef struct {
    void *ctx;
    int (*thread_create)(void *, void *(*)(void *), void *, ewrtc_thread *);
    int (*thread_join)(void *, ewrtc_thread);
    int (*thread_is_current)(void *, ewrtc_thread);
    int (*mutex_create)(void *, ewrtc_mutex *);
    void (*mutex_destroy)(void *, ewrtc_mutex);
    void (*mutex_lock)(void *, ewrtc_mutex);
    void (*mutex_unlock)(void *, ewrtc_mutex);
    int (*condition_create)(void *, ewrtc_condition *);
    void (*condition_destroy)(void *, ewrtc_condition);
    void (*condition_signal)(void *, ewrtc_condition);
    /* Relative monotonic timeout; UINT32_MAX waits indefinitely. May wake spuriously. */
    int (*condition_wait)(void *, ewrtc_condition, ewrtc_mutex, uint32_t timeout_ms);
} ewrtc_threads;
#endif
