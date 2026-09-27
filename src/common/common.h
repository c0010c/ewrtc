#ifndef EWRTC_INTERNAL_COMMON_H
#define EWRTC_INTERNAL_COMMON_H
#include "ewrtc/common.h"
#define EWRTC_MTU 1200u
static inline bool ewrtc_direction_sends(ewrtc_direction d) {
    return d == EWRTC_SENDRECV || d == EWRTC_SENDONLY;
}
static inline bool ewrtc_direction_receives(ewrtc_direction d) {
    return d == EWRTC_SENDRECV || d == EWRTC_RECVONLY;
}

int ewrtc_ascii_casecmp(const char *, const char *);
int ewrtc_ascii_ncasecmp(const char *, const char *, size_t);
/* UTC YYYYMMDDhhmmss, independent of libc timezone/global state. */
int ewrtc_utc_format(uint64_t seconds, char out[15]);
static inline uint16_t ewrtc_read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static inline uint32_t ewrtc_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void ewrtc_write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void ewrtc_write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif
