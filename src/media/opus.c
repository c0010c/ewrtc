#include "private.h"
#include <string.h>
int ewrtc_media_send_audio(ewrtc_media *s, const uint8_t *data, size_t size, uint64_t pts_us) {
    if (!s || !data || !size || size > EWRTC_MAX_OPUS_PACKET_BYTES)
        return EWRTC_INVALID;
    media_state *m = &s->media;
    if (!m->audio_started) {
        m->audio_pts_base = pts_us;
        m->audio_started = true;
    }
    if (pts_us < m->audio_pts_base)
        return EWRTC_INVALID;
    uint32_t ts = m->audio_ts_base + (uint32_t)(((pts_us - m->audio_pts_base) * 48) / 1000);
    int result = media_send_rtp(s, (uint8_t)s->cfg.audio_pt, m->audio_seq++, ts, m->audio_ssrc, false,
                          data, size, false, 2);
    if (result)
        return result;
    m->last_audio_ts = ts;
    return 0;
}

int media_receive_rtp(ewrtc_media *s, const uint8_t *packet, size_t len) {
    ewrtc_rtp_packet r;
    int result = ewrtc_rtp_parse(packet, len, &r);
    if (result)
        return result;
    if (r.payload_type == s->cfg.video_pt ||
        (s->cfg.rtx_pt >= 0 && r.payload_type == s->cfg.rtx_pt))
        return s->cfg.video ? media_receive_video(s, &r, r.payload_type == s->cfg.rtx_pt) : 0;
    if (r.payload_type == s->cfg.audio_pt && r.payload_size) {
        ++s->stats.received_audio_packets;
        if (s->cfg.audio)
            s->cfg.audio(s->cfg.user, r.payload, r.payload_size, r.timestamp, r.sequence);
    }
    return 0;
}

