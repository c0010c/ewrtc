#include "private.h"
#include <stdio.h>
#include <string.h>

void session_sync_media_stats(ewrtc_session *s) {
    if (!s->media)
        return;
    ewrtc_media_stats m;
    ewrtc_media_get_stats(s->media, &m);
    session_lock(s);
    s->stats.received_video_packets = m.received_video_packets;
    s->stats.received_video_frames = m.received_video_frames;
    s->stats.dropped_video_frames = m.dropped_video_frames;
    s->stats.sent_nacks = m.sent_nacks;
    s->stats.sent_plis = m.sent_plis;
    s->stats.received_rtx_packets = m.received_rtx_packets;
    s->stats.sent_video_packets = m.sent_video_packets;
    s->stats.sent_audio_packets = m.sent_audio_packets;
    s->stats.received_audio_packets = m.received_audio_packets;
    s->stats.nack_requests = m.nack_requests;
    s->stats.rtx_packets = m.rtx_packets;
    s->stats.pli_requests = m.pli_requests;
    s->stats.retransmit_cache_bytes = m.retransmit_cache_bytes;
    session_unlock(s);
}

void session_update_path(ewrtc_session *s) {
    char local[256] = {0}, remote[256] = {0};
    if (s->ice && ewrtc_ice_selected(s->ice, local, sizeof(local), remote, sizeof(remote)) == 0) {
        session_lock(s);
        snprintf(s->stats.local_candidate, sizeof(s->stats.local_candidate), "%s", local);
        snprintf(s->stats.remote_candidate, sizeof(s->stats.remote_candidate), "%s", remote);
        session_unlock(s);
    }
}

ewrtc_result ewrtc_session_get_stats(ewrtc_session *s, ewrtc_stats *out) {
    if (!s || !out)
        return EWRTC_INVALID;
    session_lock(s);
    *out = s->stats;
    session_unlock(s);
    return EWRTC_OK;
}

