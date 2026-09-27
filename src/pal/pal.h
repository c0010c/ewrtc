#ifndef EWRTC_INTERNAL_PAL_H
#define EWRTC_INTERNAL_PAL_H
#include "ewrtc/pal.h"
#include "common/common.h"
void *ewrtc_alloc(const ewrtc_pal *, size_t);
void *ewrtc_zalloc(const ewrtc_pal *, size_t);
void *ewrtc_resize(const ewrtc_pal *, void *, size_t);
void ewrtc_free(const ewrtc_pal *, void *);
char *ewrtc_strdup(const ewrtc_pal *, const char *);
void ewrtc_log(const ewrtc_pal *, int, const char *);
static inline uint64_t ewrtc_now_ms(const ewrtc_pal *p) {
    return p->clock.monotonic_ms(p->clock.ctx);
}
static inline uint64_t ewrtc_utc_us(const ewrtc_pal *p) {
    return p->clock.utc_us(p->clock.ctx);
}
static inline int ewrtc_random_bytes(const ewrtc_pal *p, void *b, size_t n) {
    return p->random.bytes(p->random.ctx, b, n);
}

#endif
