#ifndef EWRTC_INTERNAL_PAL_H
#define EWRTC_INTERNAL_PAL_H
#include "ewrtc/pal.h"
#include "common/common.h"
#include <stdarg.h>
void *ewrtc_alloc(const ewrtc_pal *, size_t);
void *ewrtc_zalloc(const ewrtc_pal *, size_t);
void *ewrtc_resize(const ewrtc_pal *, void *, size_t);
void ewrtc_free(const ewrtc_pal *, void *);
char *ewrtc_strdup(const ewrtc_pal *, const char *);
int ewrtc_log_enabled(const ewrtc_pal *, int);
#if defined(__GNUC__) || defined(__clang__)
#define EWRTC_PRINTF(fmt_index, arg_index) __attribute__((format(printf, fmt_index, arg_index)))
#else
#define EWRTC_PRINTF(format, args)
#endif
void ewrtc_log_write(const ewrtc_pal *, int, const char *module, const char *scope,
                     const char *format, ...) EWRTC_PRINTF(5, 6);
void ewrtc_log_vwrite(const ewrtc_pal *, int, const char *module, const char *scope,
                      const char *format, va_list) EWRTC_PRINTF(5, 0);
/* Keep argument evaluation behind the filter, including expensive diagnostics. */
#define EWRTC_LOG(pal, level, module, ...) do { \
    if (ewrtc_log_enabled((pal), (level))) \
        ewrtc_log_write((pal), (level), (module), "-", __VA_ARGS__); \
} while (0)
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
