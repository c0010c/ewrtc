#include "private.h"
#include <string.h>
int media_receive_rtcp(ewrtc_media *s, const uint8_t *packet, size_t len) {
    size_t off = 0;
    ewrtc_rtcp_packet block;
    int result;
    while ((result = ewrtc_rtcp_next(packet, len, &off, &block)) == 0) {
        const uint8_t *p = block.bytes;
        size_t plen = block.size;
        unsigned fmt = block.count;
        if (p[1] == 200 && plen >= 28 && ewrtc_read_u32(p + 4) == s->receiver.ssrc) {
            s->receiver.lsr = (ewrtc_read_u32(p + 8) << 16) | (ewrtc_read_u32(p + 12) >> 16);
            s->receiver.sr_at = ewrtc_now_ms(&s->cfg.pal);
            s->receiver.have_sr = true;
        }
        if (p[1] == 205 && fmt == 1 && plen >= 16 && ewrtc_read_u32(p + 8) == s->media.video_ssrc) {
            for (size_t at = 12; at + 4 <= plen; at += 4) {
                uint16_t pid = ewrtc_read_u16(p + at), blp = ewrtc_read_u16(p + at + 2);
                s->stats.nack_requests++;
                int sent = media_send_rtx(s, pid);
                if (sent) return sent;
                for (unsigned bit = 0; bit < 16; ++bit)
                    if (blp & (1u << bit)) {
                        sent = media_send_rtx(s, (uint16_t)(pid + bit + 1));
                        if (sent) return sent;
                    }
            }
        } else if (p[1] == 206 &&
                   ((fmt == 1 && plen >= 12 && ewrtc_read_u32(p + 8) == s->media.video_ssrc) ||
                    (fmt == 4 && plen >= 20 && ewrtc_read_u32(p + 12) == s->media.video_ssrc))) {
            s->stats.pli_requests++;
            if (s->cfg.keyframe)
                s->cfg.keyframe(s->cfg.user);
        }
    }
    return result == EWRTC_AGAIN ? 0 : result;
}

static int send_sr(ewrtc_media *s, uint32_t ssrc, uint32_t rtp_ts, uint32_t packets,
                    uint32_t octets) {
    uint8_t packet[28];
    if (ewrtc_rtcp_write_sr(ssrc, rtp_ts, packets, octets, ewrtc_utc_us(&s->cfg.pal), packet,
                            sizeof(packet)))
        return EWRTC_INVALID;
    return media_send_plain(s, packet, 28, true, 0);
}

static int media_tick(ewrtc_media *s) {
    if (!s)
        return EWRTC_INVALID;
    media_state *m = &s->media;
    media_evict_cache(s);
    int receive_result = media_receiver_tick(s);
    if (receive_result) return receive_result;
    uint64_t now = ewrtc_now_ms(&s->cfg.pal);
    if (now - m->last_rtcp_ms >= 1000) {
        int result;
        if (m->video_started && (result = send_sr(s, m->video_ssrc, m->last_video_ts, m->video_packets, m->video_octets)))
            return result;
        if (m->audio_started && (result = send_sr(s, m->audio_ssrc, m->last_audio_ts, m->audio_packets, m->audio_octets)))
            return result;
        if (s->receiver.started) {
            uint8_t report[52]; size_t size;
            media_receiver_report(s, report, &size);
            result = media_send_plain(s, report, size, true, 0);
            if (result) return result;
        }
        m->last_rtcp_ms = now;
    }
    return EWRTC_OK;
}


int ewrtc_media_tick(ewrtc_media *s) {
    if (!s) return EWRTC_INVALID;
    int result = media_tick(s);
    s->retry_at = result == EWRTC_AGAIN || result == EWRTC_BACKPRESSURE
                    ? ewrtc_now_ms(&s->cfg.pal) + 20 : 0;
    return result;
}
uint64_t ewrtc_media_next_deadline(ewrtc_media *s) {
    if (!s) return UINT64_MAX;
    uint64_t next = media_receiver_deadline(s);
    media_state *m = &s->media;
    if (m->video_started || m->audio_started || s->receiver.started) {
        uint64_t at = m->last_rtcp_ms + 1000;
        if (at < next) next = at;
    }
    if (m->cache_count) {
        uint64_t at = m->cache[0].at_ms + m->cache_age_ms + 1;
        if (at < next) next = at;
    }
    if (s->retry_at && next < s->retry_at) next = s->retry_at;
    return next;
}
