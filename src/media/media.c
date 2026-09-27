#include "private.h"
#include <string.h>
int ewrtc_media_receive(ewrtc_media *s, const uint8_t *wire, size_t size, bool rtcp) {
    if (!s || !wire || size < 4 || size > 2048)
        return EWRTC_INVALID;
    return rtcp ? media_receive_rtcp(s, wire, size) : media_receive_rtp(s, wire, size);
}

int ewrtc_media_create(const ewrtc_media_config *c, ewrtc_media **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!c || !c->send || c->video_pt < 0 || c->video_pt > 127 || c->audio_pt < 0 ||
        c->audio_pt > 127 || c->rtx_pt < -1 || c->rtx_pt > 127 || c->video_pt == c->audio_pt ||
        c->rtx_pt == c->video_pt || c->rtx_pt == c->audio_pt ||
        ewrtc_pal_validate(&c->pal, EWRTC_PAL_MEMORY | EWRTC_PAL_CLOCK | EWRTC_PAL_RANDOM))
        return EWRTC_INVALID;
    ewrtc_media *s = ewrtc_zalloc(&c->pal, sizeof(*s));
    if (!s)
        return EWRTC_NOMEM;
    s->cfg = *c;
    s->receiver.ssrc = c->remote_video_ssrc;
    s->receiver.rtx_ssrc = c->remote_rtx_ssrc;
    media_state *m = &s->media;
    m->video_ssrc = c->video_ssrc;
    m->rtx_ssrc = c->rtx_ssrc;
    m->audio_ssrc = c->audio_ssrc;
    m->cache_limit = c->cache_limit_bytes ? c->cache_limit_bytes : 2 * 1024 * 1024;
    m->cache_age_ms = c->cache_max_age_ms ? c->cache_max_age_ms : 2000;
    uint32_t random[5];
    if (ewrtc_random_bytes(&c->pal, random, sizeof(random))) {
        ewrtc_free(&c->pal, s);
        return EWRTC_SECURITY;
    }
    m->video_seq = (uint16_t)random[0];
    m->rtx_seq = (uint16_t)random[1];
    m->audio_seq = (uint16_t)random[2];
    m->video_ts_base = random[3];
    m->audio_ts_base = random[4];
    *out = s;
    return 0;
}

void ewrtc_media_destroy(ewrtc_media *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->media.cache_count; ++i)
        ewrtc_free(&s->cfg.pal, s->media.cache[i].rtp);
    ewrtc_free(&s->cfg.pal, s->media.cache);
    media_receiver_destroy(s);
    ewrtc_free(&s->cfg.pal, s);
}

int ewrtc_media_get_stats(const ewrtc_media *s, ewrtc_media_stats *out) {
    if (!s || !out)
        return EWRTC_INVALID;
    *out = s->stats;
    return 0;
}

