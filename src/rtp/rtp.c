#include "rtp/rtp.h"
#include <string.h>

int ewrtc_rtp_parse(const uint8_t *p, size_t n, ewrtc_rtp_packet *out) {
    if (!p || !out || n < 12 || (p[0] >> 6) != 2)
        return EWRTC_INVALID;
    size_t h = 12 + (size_t) (p[0] & 15) * 4;
    if (h > n)
        return EWRTC_INVALID;
    if (p[0] & 16) {
        if (n - h < 4)
            return EWRTC_INVALID;
        h += 4 + (size_t) ewrtc_read_u16(p + h + 2) * 4;
        if (h > n)
            return EWRTC_INVALID;
    }
    size_t end = n;
    if (p[0] & 32) {
        size_t padding = p[n - 1];
        if (!padding || padding > n - h)
            return EWRTC_INVALID;
        end -= padding;
    }
    *out = (ewrtc_rtp_packet){
        p[1] & 127,
        (p[1] & 128) != 0,
        ewrtc_read_u16(p + 2),
        ewrtc_read_u32(p + 4),
        ewrtc_read_u32(p + 8),
        p + h,
        end - h
    };
    return 0;
}

int ewrtc_rtp_write(const ewrtc_rtp_packet *r, uint8_t *p, size_t cap, size_t *n) {
    if (!r || !p || !n || r->payload_type > 127 || cap < 12 || r->payload_size > cap - 12 ||
        (!r->payload && r->payload_size))
        return EWRTC_INVALID;
    p[0] = 128;
    p[1] = r->payload_type | (r->marker ? 128 : 0);
    ewrtc_write_u16(p + 2, r->sequence);
    ewrtc_write_u32(p + 4, r->timestamp);
    ewrtc_write_u32(p + 8, r->ssrc);
    if (r->payload_size)
        memcpy(p + 12, r->payload, r->payload_size);
    *n = 12 + r->payload_size;
    return 0;
}

int ewrtc_rtcp_next(const uint8_t *p, size_t n, size_t *off, ewrtc_rtcp_packet *out) {
    if (!p || !off || !out || *off > n)
        return EWRTC_INVALID;
    if (*off == n)
        return EWRTC_AGAIN;
    if (n - *off < 4)
        return EWRTC_INVALID;
    const uint8_t *b = p + *off;
    size_t size = ((size_t) ewrtc_read_u16(b + 2) + 1) * 4;
    if ((b[0] >> 6) != 2 || size > n - *off)
        return EWRTC_INVALID;
    size_t content = size;
    if (b[0] & 32) {
        unsigned pad = b[size - 1];
        if (*off + size != n || !pad || pad > size - 4)
            return EWRTC_INVALID;
        content -= pad;
    }
    *out = (ewrtc_rtcp_packet){b[1], b[0] & 31, b, content};
    *off += size;
    return 0;
}

int ewrtc_rtcp_write_sr(uint32_t ssrc, uint32_t ts, uint32_t packets, uint32_t octets, uint64_t utc,
                        uint8_t *p, size_t cap) {
    if (!p || cap < 28)
        return EWRTC_INVALID;
    memset(p, 0, 28);
    p[0] = 128;
    p[1] = 200;
    ewrtc_write_u16(p + 2, 6);
    ewrtc_write_u32(p + 4, ssrc);
    ewrtc_write_u32(p + 8, (uint32_t) (utc / 1000000) + 2208988800u);
    ewrtc_write_u32(p + 12, (uint32_t) (((utc % 1000000) << 32) / 1000000));
    ewrtc_write_u32(p + 16, ts);
    ewrtc_write_u32(p + 20, packets);
    ewrtc_write_u32(p + 24, octets);
    return 0;
}

int ewrtc_rtcp_write_feedback(uint8_t type, uint8_t fmt, uint32_t sender, uint32_t media,
                              const uint8_t *fci, size_t fn, uint8_t *p, size_t cap, size_t *n) {
    if (!p || !n || (type != 205 && type != 206) || fmt > 31 || fn % 4 || fn > 262128 || cap < 12 ||
        fn > cap - 12 || (!fci && fn))
        return EWRTC_INVALID;
    p[0] = 128 | fmt;
    p[1] = type;
    ewrtc_write_u16(p + 2, (uint16_t) ((12 + fn) / 4 - 1));
    ewrtc_write_u32(p + 4, sender);
    ewrtc_write_u32(p + 8, media);
    if (fn)
        memcpy(p + 12, fci, fn);
    *n = 12 + fn;
    return 0;
}
