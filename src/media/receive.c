#include "private.h"
#include <string.h>

#define REORDER_WAIT_MS 150u
#define NACK_INTERVAL_MS 30u
#define PLI_INTERVAL_MS 500u

static void clear_packets(ewrtc_media *s) {
    for (unsigned i = 0; i < VIDEO_REORDER_PACKETS; ++i) {
        ewrtc_free(&s->cfg.pal, s->receiver.packets[i]);
        s->receiver.packets[i] = NULL;
    }
}
void media_receiver_destroy(ewrtc_media *s) {
    clear_packets(s);
    ewrtc_free(&s->cfg.pal, s->receiver.frame);
}
static void break_frame(ewrtc_media *s) {
    video_receiver *r = &s->receiver;
    if (!r->broken) ++s->stats.dropped_video_frames;
    r->broken = true;
    r->fu_active = false;
    r->size = 0;
    r->pli_pending = true;
}
static int append_bytes(ewrtc_media *s, const uint8_t *data, size_t size) {
    video_receiver *r = &s->receiver;
    if (size > EWRTC_MAX_FRAME - r->size) return EWRTC_INVALID;
    size_t need = r->size + size;
    if (need > r->capacity) {
        size_t cap = r->capacity ? r->capacity : 4096;
        while (cap < need) cap *= 2;
        if (cap > EWRTC_MAX_FRAME) cap = EWRTC_MAX_FRAME;
        uint8_t *p = ewrtc_resize(&s->cfg.pal, r->frame, cap);
        if (!p) return EWRTC_NOMEM;
        r->frame = p;
        r->capacity = cap;
    }
    memcpy(r->frame + r->size, data, size);
    r->size += size;
    return 0;
}
static int append_nal(ewrtc_media *s, const uint8_t *p, size_t n) {
    static const uint8_t prefix[] = {0, 0, 0, 1};
    if (!n || (p[0] & 0x80) || !(p[0] & 31) || (p[0] & 31) >= 24) return EWRTC_INVALID;
    int result = append_bytes(s, prefix, sizeof(prefix));
    if (!result) result = append_bytes(s, p, n);
    if ((p[0] & 31) == 5) s->receiver.keyframe = true;
    return result;
}
static int depacketize(ewrtc_media *s, const video_packet *p) {
    video_receiver *r = &s->receiver;
    const uint8_t *data = p->data;
    size_t size = p->size;
    unsigned type = data[0] & 31;
    if (data[0] & 0x80) return EWRTC_INVALID;
    if (type >= 1 && type <= 23) {
        if (r->fu_active) return EWRTC_INVALID;
        return append_nal(s, data, size);
    }
    if (type == 24) {
        if (r->fu_active || size < 4) return EWRTC_INVALID;
        for (size_t at = 1; at < size;) {
            if (size - at < 2) return EWRTC_INVALID;
            size_t n = ewrtc_read_u16(data + at);
            at += 2;
            if (!n || n > size - at) return EWRTC_INVALID;
            int result = append_nal(s, data + at, n);
            if (result) return result;
            at += n;
        }
        return 0;
    }
    if (type != 28 || size < 3 || (data[1] & 0x20)) return EWRTC_INVALID;
    bool start = (data[1] & 0x80) != 0, end = (data[1] & 0x40) != 0;
    uint8_t header = (data[0] & 0xe0) | (data[1] & 31);
    if (!(header & 31) || (header & 31) >= 24 || (start && end)) return EWRTC_INVALID;
    if (start) {
        if (r->fu_active) return EWRTC_INVALID;
        int result = append_nal(s, &header, 1);
        if (result) return result;
        r->fu_active = true;
        r->fu_header = header;
    } else if (!r->fu_active || r->fu_header != header) return EWRTC_INVALID;
    int result = append_bytes(s, data + 2, size - 2);
    if (end) r->fu_active = false;
    return result;
}
static int consume(ewrtc_media *s, const video_packet *p) {
    video_receiver *r = &s->receiver;
    if (r->frame_active && p->timestamp != r->timestamp) {
        break_frame(s); /* previous AU never received its marker */
        r->frame_active = false;
    }
    if (!r->frame_active) {
        /* Never deliver a second AU from late packets with the same timestamp. */
        if (r->have_last_timestamp && p->timestamp == r->last_timestamp) return 0;
        r->timestamp = p->timestamp;
        r->frame_active = true;
        r->frame_at = ewrtc_now_ms(&s->cfg.pal);
        r->size = 0;
        r->fu_active = r->broken = r->keyframe = false;
    }
    int result = r->broken ? 0 : depacketize(s, p);
    if (result) break_frame(s);
    if (p->marker) {
        if (r->fu_active) break_frame(s);
        if (!r->broken && r->size) {
            ++s->stats.received_video_frames;
            if (r->keyframe) r->pli_pending = false;
            s->cfg.video(s->cfg.user, r->frame, r->size, r->timestamp, r->keyframe);
        }
        r->frame_active = false;
        r->last_timestamp = p->timestamp;
        r->have_last_timestamp = true;
        r->size = 0;
    }
    return result;
}
static int drain(ewrtc_media *s) {
    video_receiver *r = &s->receiver;
    int error = 0;
    for (;;) {
        unsigned slot = r->next % VIDEO_REORDER_PACKETS;
        video_packet *p = r->packets[slot];
        if (!p || p->sequence != r->next) break;
        r->packets[slot] = NULL;
        ++r->next;
        int result = consume(s, p);
        if (result) error = result;
        ewrtc_free(&s->cfg.pal, p);
    }
    bool pending = false;
    for (unsigned i = 0; i < VIDEO_REORDER_PACKETS; ++i)
        if (r->packets[i]) pending = true;
    if (pending && !r->gap_pending) {
        r->gap_pending = true;
        r->gap_at = ewrtc_now_ms(&s->cfg.pal);
        r->nack_at = 0;
    } else if (!pending) r->gap_pending = false;
    return error;
}
int media_receive_video(ewrtc_media *s, const ewrtc_rtp_packet *packet, bool rtx) {
    video_receiver *r = &s->receiver;
    ewrtc_rtp_packet p = *packet;
    if (rtx) {
        if (p.payload_size <= 2 || !r->ssrc || !r->started ||
            (r->rtx_ssrc && p.ssrc != r->rtx_ssrc)) return EWRTC_INVALID;
        if (!r->rtx_ssrc) r->rtx_ssrc = p.ssrc;
        p.sequence = ewrtc_read_u16(p.payload);
        p.payload += 2;
        p.payload_size -= 2;
        ++s->stats.received_rtx_packets;
    } else {
        if (!p.payload_size || (r->ssrc && p.ssrc != r->ssrc)) return EWRTC_INVALID;
        r->ssrc = p.ssrc;
    }
    if (!r->started) {
        r->started = true;
        r->next = p.sequence;
        r->highest = r->base_sequence = p.sequence;
    }
    int16_t distance = (int16_t)(p.sequence - r->next);
    if (distance < 0) return 0; /* duplicate or outside the reorder window */
    if (distance >= (int)VIDEO_REORDER_PACKETS) {
        break_frame(s);
        clear_packets(s);
        /* The first AU after a discontinuity may have lost its initial NALs.
         * Discard it through its marker before admitting the next AU. */
        r->timestamp = p.timestamp;
        r->frame_active = true;
        r->next = p.sequence;
        r->gap_pending = false;
    }
    unsigned slot = p.sequence % VIDEO_REORDER_PACKETS;
    if (r->packets[slot]) return 0;
    video_packet *saved = ewrtc_alloc(&s->cfg.pal, sizeof(*saved) + p.payload_size);
    if (!saved) return EWRTC_NOMEM;
    *saved = (video_packet){.sequence = p.sequence, .timestamp = p.timestamp,
                           .marker = p.marker, .size = p.payload_size};
    memcpy(saved->data, p.payload, p.payload_size);
    r->packets[slot] = saved;
    if (!rtx) {
        uint32_t transit = (uint32_t)(ewrtc_now_ms(&s->cfg.pal) * 90) - p.timestamp;
        if (r->have_transit) {
            int64_t delta = (int32_t)(transit - r->transit);
            if (delta < 0) delta = -delta;
            r->jitter_q4 += delta - ((r->jitter_q4 + 8) / 16);
        }
        r->transit = transit;
        r->have_transit = true;
    }
    ++r->received;
    ++s->stats.received_video_packets;
    int16_t advance = (int16_t)(p.sequence - (uint16_t)r->highest);
    if (advance > 0) r->highest += (uint16_t)advance;
    return drain(s);
}
/* Prefix feedback with RR + SDES so reduced-size RTCP need not be negotiated. */
int media_receiver_report(ewrtc_media *s, uint8_t *out, size_t *size) {
    video_receiver *r = &s->receiver;
    memset(out, 0, 52);
    out[0] = r->started ? 0x81 : 0x80;
    out[1] = 201;
    ewrtc_write_u16(out + 2, r->started ? 7 : 1);
    ewrtc_write_u32(out + 4, s->media.video_ssrc);
    size_t n = 8;
    if (r->started) {
        uint32_t expected = r->highest - r->base_sequence + 1;
        int64_t lost = (int64_t)expected - r->received;
        if (lost < 0) lost = 0;
        if (lost > 0x7fffff) lost = 0x7fffff;
        uint32_t interval = expected - r->expected_prior;
        int64_t missing = (int64_t)interval - (r->received - r->received_prior);
        uint32_t fraction = interval && missing > 0 ? (uint32_t)(missing * 256 / interval) : 0;
        if (fraction > 255) fraction = 255;
        ewrtc_write_u32(out + 8, r->ssrc);
        ewrtc_write_u32(out + 12, (fraction << 24) | (uint32_t)lost);
        ewrtc_write_u32(out + 16, r->highest);
        ewrtc_write_u32(out + 20, (uint32_t)(r->jitter_q4 / 16));
        ewrtc_write_u32(out + 24, r->lsr);
        if (r->have_sr) ewrtc_write_u32(out + 28, (uint32_t)((ewrtc_now_ms(&s->cfg.pal) - r->sr_at) * 65536 / 1000));
        r->expected_prior = expected;
        r->received_prior = r->received;
        n = 32;
    }
    out[n] = 0x81; out[n + 1] = 202;
    ewrtc_write_u16(out + n + 2, 3);
    ewrtc_write_u32(out + n + 4, s->media.video_ssrc);
    out[n + 8] = 1; out[n + 9] = 5;
    memcpy(out + n + 10, "ewrtc", 5);
    *size = n + 16;
    return 0;
}
static int feedback(ewrtc_media *s, bool pli) {
    uint8_t bytes[68];
    size_t n;
    media_receiver_report(s, bytes, &n);
    memset(bytes + n, 0, 16);
    bytes[n] = 0x81; bytes[n + 1] = pli ? 206 : 205;
    ewrtc_write_u16(bytes + n + 2, pli ? 2 : 3);
    ewrtc_write_u32(bytes + n + 4, s->media.video_ssrc);
    ewrtc_write_u32(bytes + n + 8, s->receiver.ssrc);
    if (!pli) {
        video_receiver *r = &s->receiver;
        ewrtc_write_u16(bytes + n + 12, r->next);
        uint16_t mask = 0;
        unsigned distance = (uint16_t)((uint16_t)r->highest - r->next);
        for (unsigned i = 1; i <= 16 && i <= distance; ++i) {
            uint16_t seq = (uint16_t)(r->next + i);
            video_packet *p = r->packets[seq % VIDEO_REORDER_PACKETS];
            if (!p || p->sequence != seq) mask |= (uint16_t)(1u << (i - 1));
        }
        ewrtc_write_u16(bytes + n + 14, mask);
    }
    int result = media_send_plain(s, bytes, n + (pli ? 12 : 16), true, 0);
    if (!result) {
        if (pli) ++s->stats.sent_plis;
        else ++s->stats.sent_nacks;
    }
    return result;
}
int media_receiver_tick(ewrtc_media *s) {
    video_receiver *r = &s->receiver;
    uint64_t now = ewrtc_now_ms(&s->cfg.pal);
    if (r->gap_pending) {
        if (now - r->gap_at >= REORDER_WAIT_MS) {
            break_frame(s);
            /* Skip the damaged AU (including buffered packets from that AU). */
            unsigned nearest = VIDEO_REORDER_PACKETS;
            video_packet *next = NULL;
            for (unsigned i = 0; i < VIDEO_REORDER_PACKETS; ++i) {
                video_packet *p = r->packets[i];
                unsigned delta = p ? (uint16_t)(p->sequence - r->next) : VIDEO_REORDER_PACKETS;
                if (delta < nearest) { nearest = delta; next = p; }
            }
            r->gap_pending = false;
            if (next) {
                r->next = next->sequence;
                r->timestamp = next->timestamp;
                r->frame_active = true;
                r->frame_at = now;
                r->gap_pending = false;
                int result = drain(s);
                if (result) return result;
            }
        } else if (s->cfg.nack && (!r->nack_at || now - r->nack_at >= NACK_INTERVAL_MS)) {
            int result = feedback(s, false);
            if (result) return result;
            r->nack_at = now;
        }
    }
    if (r->frame_active && !r->broken && now - r->frame_at >= REORDER_WAIT_MS) break_frame(s);
    if (r->pli_pending && r->ssrc && s->cfg.pli && (!r->pli_at || now - r->pli_at >= PLI_INTERVAL_MS)) {
        int result = feedback(s, true);
        if (result) return result;
        r->pli_at = now;
    }
    return 0;
}

uint64_t media_receiver_deadline(ewrtc_media *s) {
    video_receiver *r = &s->receiver;
    uint64_t next = UINT64_MAX, at;
    if (r->gap_pending) {
        next = r->gap_at + REORDER_WAIT_MS;
        if (s->cfg.nack) {
            at = r->nack_at ? r->nack_at + NACK_INTERVAL_MS : 0;
            if (at < next) next = at;
        }
    }
    if (r->frame_active && !r->broken) {
        at = r->frame_at + REORDER_WAIT_MS;
        if (at < next) next = at;
    }
    if (r->pli_pending && r->ssrc && s->cfg.pli) {
        at = r->pli_at ? r->pli_at + PLI_INTERVAL_MS : 0;
        if (at < next) next = at;
    }
    return next;
}
