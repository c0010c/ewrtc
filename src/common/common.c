#include "common/common.h"
#include <stdio.h>
#include <string.h>
int ewrtc_address_parse(const char *s, uint16_t port, ewrtc_address *a) {
    if (!s || !a)
        return EWRTC_INVALID;
    uint32_t ip = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return EWRTC_INVALID;
        }
        if (!digits || (i < 3 ? *s++ != '.' : *s != 0))
            return EWRTC_INVALID;
        ip = (ip << 8) | v;
    }
    *a = (ewrtc_address){ip, port};
    return EWRTC_OK;
}
int ewrtc_address_format(const ewrtc_address *a, char *s, size_t n) {
    if (!a || !s || !n)
        return EWRTC_INVALID;
    int r = snprintf(s, n, "%u.%u.%u.%u", a->ipv4 >> 24, (a->ipv4 >> 16) & 255,
                     (a->ipv4 >> 8) & 255, a->ipv4 & 255);
    return r < 0 || (size_t)r >= n ? EWRTC_INVALID : EWRTC_OK;
}
static unsigned lower(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
int ewrtc_ascii_ncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        unsigned x = lower((unsigned char)*a++), y = lower((unsigned char)*b++);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            break;
    }
    return 0;
}
int ewrtc_ascii_casecmp(const char *a, const char *b) {
    return ewrtc_ascii_ncasecmp(a, b, SIZE_MAX);
}
int ewrtc_utc_format(uint64_t t, char out[15]) {
    if (!out || t > 253402300799ULL)
        return EWRTC_INVALID;
    uint64_t days = t / 86400;
    unsigned year = 1970, month = 1;
    for (;;) {
        unsigned n = 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (days < n)
            break;
        days -= n;
        ++year;
    }
    static const unsigned months[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (;;) {
        unsigned n = months[month - 1] +
                     (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (days < n)
            break;
        days -= n;
        ++month;
    }
    return snprintf(out, 15, "%04u%02u%02u%02u%02u%02u", year, month, (unsigned)days + 1,
                    (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60), (unsigned)(t % 60)) == 14
               ? 0
               : EWRTC_INVALID;
}
