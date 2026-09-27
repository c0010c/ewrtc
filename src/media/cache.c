#include "private.h"
#include <string.h>
static void update_cache_bytes(ewrtc_media *s) {
    s->stats.retransmit_cache_bytes = s->media.cache_bytes;
}

void media_evict_cache(ewrtc_media *s) {
    media_state *m = &s->media;
    uint64_t now = ewrtc_now_ms(&s->cfg.pal);
    while (m->cache_count &&
           (m->cache_bytes > m->cache_limit || now - m->cache[0].at_ms > m->cache_age_ms)) {
        m->cache_bytes -= m->cache[0].length;
        ewrtc_free(&s->cfg.pal, m->cache[0].rtp);
        memmove(m->cache, m->cache + 1, (--m->cache_count) * sizeof(*m->cache));
    }
    update_cache_bytes(s);
}

int media_add_cache(ewrtc_media *s, const uint8_t *rtp, size_t len, uint16_t seq,
                     uint32_t timestamp) {
    media_state *m = &s->media;
    if (len > m->cache_limit)
        return EWRTC_BACKPRESSURE;
    media_evict_cache(s);
    if (m->cache_count == m->cache_capacity) {
        size_t next = m->cache_capacity ? m->cache_capacity * 2 : 128;
        if (next > 4096)
            next = 4096;
        if (next == m->cache_capacity)
            return EWRTC_BACKPRESSURE;
        retransmit_entry *p = ewrtc_resize(&s->cfg.pal, m->cache, next * sizeof(*p));
        if (!p)
            return EWRTC_NOMEM;
        m->cache = p;
        m->cache_capacity = next;
    }
    uint8_t *copy = ewrtc_alloc(&s->cfg.pal, len);
    if (!copy)
        return EWRTC_NOMEM;
    memcpy(copy, rtp, len);
    m->cache[m->cache_count++] =
        (retransmit_entry){seq, timestamp, ewrtc_now_ms(&s->cfg.pal), copy, len};
    m->cache_bytes += len;
    media_evict_cache(s);
    return 0;
}

int media_send_rtx(ewrtc_media *s, uint16_t original_seq) {
    media_state *m = &s->media;
    media_evict_cache(s);
    for (size_t i = m->cache_count; i > 0; --i) {
        retransmit_entry *entry = &m->cache[i - 1];
        if (entry->seq != original_seq)
            continue;
        if (s->cfg.rtx_pt < 0)
            return media_send_plain(s, entry->rtp, entry->length, false, 0);
        uint8_t payload[EWRTC_MTU];
        size_t original_payload_len = entry->length - 12;
        if (original_payload_len + 2 > sizeof(payload))
            return EWRTC_OK;
        ewrtc_write_u16(payload, original_seq);
        memcpy(payload + 2, entry->rtp + 12, original_payload_len);
        bool marker = (entry->rtp[1] & 0x80) != 0;
        int result = media_send_rtp(s, (uint8_t)s->cfg.rtx_pt, m->rtx_seq++, entry->timestamp, m->rtx_ssrc,
                      marker, payload, original_payload_len + 2, false, 0);
        if (!result) {
            s->stats.rtx_packets++;
        }
        return result;
    }
    return EWRTC_OK;
}

