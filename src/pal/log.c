#include "pal.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifndef EWRTC_LOG_MIN_LEVEL
#define EWRTC_LOG_MIN_LEVEL 1
#endif

int ewrtc_log_enabled(const ewrtc_pal *p, int level) {
    return p && p->log.write && level >= EWRTC_LOG_MIN_LEVEL && level <= EWRTC_LOG_ERROR;
}

void ewrtc_log_vwrite(const ewrtc_pal *p, int level, const char *module, const char *scope,
                      const char *format, va_list args) {
    if (!ewrtc_log_enabled(p, level)) return;
    static const char *const levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char stamp[40], raw[768], line[1024];
    if (p->clock.utc_us) {
        uint64_t us = p->clock.utc_us(p->clock.ctx);
        char date[15];
        if (!ewrtc_utc_format(us / 1000000, date))
            snprintf(stamp, sizeof(stamp), "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s.%03uZ",
                     date, date + 4, date + 6, date + 8, date + 10, date + 12,
                     (unsigned)(us / 1000 % 1000));
        else snprintf(stamp, sizeof(stamp), "utc-unavailable");
    } else if (p->clock.monotonic_ms) {
        snprintf(stamp, sizeof(stamp), "+%" PRIu64 "ms", p->clock.monotonic_ms(p->clock.ctx));
    } else snprintf(stamp, sizeof(stamp), "time-unavailable");
    int n = vsnprintf(raw, sizeof(raw), format, args);
    if (n < 0) snprintf(raw, sizeof(raw), "format-error");
    bool truncated = n >= (int)sizeof(raw);
    int prefix = snprintf(line, sizeof(line), "%s %-5s %-7s %-12s ", stamp, levels[level], module, scope);
    if (prefix < 0 || (size_t)prefix >= sizeof(line) - 16) return;
    size_t used = (size_t)prefix;
    /* Escape ASCII controls so remote/backend text cannot inject lines or ANSI. */
    for (size_t i = 0; raw[i]; ++i) {
        unsigned char c = (unsigned char)raw[i];
        size_t need = c < 32 || c == 127 ? 4 : 1;
        if (used + need >= sizeof(line) - 16) { truncated = true; break; }
        if (need == 4) {
            snprintf(line + used, 5, "\\x%02x", c);
            used += 4;
        } else line[used++] = (char)c;
    }
    if (truncated) {
        memcpy(line + used, " [truncated]", 12);
        used += 12;
    }
    line[used] = 0;
    p->log.write(p->log.ctx, level, line);
}

void ewrtc_log_write(const ewrtc_pal *p, int level, const char *module, const char *scope,
                     const char *format, ...) {
    va_list args;
    va_start(args, format);
    ewrtc_log_vwrite(p, level, module, scope, format, args);
    va_end(args);
}
