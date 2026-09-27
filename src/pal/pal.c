#include "pal/pal.h"
#include <string.h>
int ewrtc_pal_validate(const ewrtc_pal *p, unsigned c) {
    if (!p || (c & ~((1u << 13) - 1)))
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_ALLOCATOR) && (!p->memory.allocate || !p->memory.release))
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_RESIZE) && !p->memory.resize)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_MONOTONIC) && !p->clock.monotonic_ms)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_UTC) && !p->clock.utc_us)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_RANDOM) && !p->random.bytes)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_EVENTS) && (!p->events.create || !p->events.destroy ||
        !p->events.add || !p->events.remove || !p->events.wait || !p->events.wake))
        return EWRTC_INVALID;
    const ewrtc_threads *t = &p->threads;
    if ((c & EWRTC_PAL_MUTEX) && (!t->mutex_create || !t->mutex_destroy || !t->mutex_lock || !t->mutex_unlock))
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_THREAD) && (!t->thread_create || !t->thread_join || !t->thread_is_current))
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_CONDITION) && (!t->condition_create || !t->condition_destroy || !t->condition_signal || !t->condition_wait))
        return EWRTC_INVALID;
    const ewrtc_network *n = &p->network;
    if ((c & EWRTC_PAL_UDP_SEND) && !n->udp_send)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_UDP_SOCKET) && (!n->udp_open || !n->udp_close || !n->udp_local || !n->udp_receive))
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_DNS) && !n->resolve)
        return EWRTC_INVALID;
    if ((c & EWRTC_PAL_INTERFACES) && !n->interfaces)
        return EWRTC_INVALID;
    return EWRTC_OK;
}
void *ewrtc_alloc(const ewrtc_pal *p, size_t n) {
    return p->memory.allocate(p->memory.ctx, n);
}
void *ewrtc_zalloc(const ewrtc_pal *p, size_t n) {
    void *r = ewrtc_alloc(p, n);
    if (r)
        memset(r, 0, n);
    return r;
}
void *ewrtc_resize(const ewrtc_pal *p, void *v, size_t n) {
    return p->memory.resize(p->memory.ctx, v, n);
}
void ewrtc_free(const ewrtc_pal *p, void *v) {
    if (v)
        p->memory.release(p->memory.ctx, v);
}
char *ewrtc_strdup(const ewrtc_pal *p, const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *r = ewrtc_alloc(p, n);
    if (r)
        memcpy(r, s, n);
    return r;
}
void ewrtc_log(const ewrtc_pal *p, int level, const char *s) {
    if (p->log.write)
        p->log.write(p->log.ctx, level, s);
}
