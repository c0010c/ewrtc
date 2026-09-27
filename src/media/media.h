#ifndef EWRTC_MEDIA_H
#define EWRTC_MEDIA_H
#include "pal/pal.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ewrtc_media ewrtc_media;
typedef struct {
    ewrtc_pal pal;
    int video_pt, rtx_pt, audio_pt;
    uint32_t video_ssrc, rtx_ssrc, audio_ssrc;
    size_t cache_limit_bytes;
    uint32_t cache_max_age_ms;
    int (*send)(void *, const uint8_t *, size_t, bool rtcp, int kind);
    void (*audio)(void *, const uint8_t *, size_t, uint32_t, uint16_t);
    void (*keyframe)(void *);
    void *user;
    void (*video)(void *, const uint8_t *, size_t, uint32_t, int);
    uint32_t remote_video_ssrc, remote_rtx_ssrc; /* zero: learn primary SSRC */
    bool nack, pli; /* negotiated receive feedback */
} ewrtc_media_config;
typedef struct {
    uint64_t sent_video_packets, sent_audio_packets, received_audio_packets;
    uint64_t nack_requests, rtx_packets, pli_requests;
    size_t retransmit_cache_bytes;
    uint64_t received_video_packets, received_video_frames, dropped_video_frames;
    uint64_t sent_nacks, sent_plis, received_rtx_packets;
} ewrtc_media_stats;
/* Caller serializes calls; send callback synchronously consumes plaintext.
 * kind: 1 video, 2 audio, 0 RTX/control. No dependency on SRTP or ICE. */
int ewrtc_media_create(const ewrtc_media_config *, ewrtc_media **);
void ewrtc_media_destroy(ewrtc_media *);
int ewrtc_media_validate_video(const uint8_t *, size_t, int keyframe);
int ewrtc_media_send_video(ewrtc_media *, const uint8_t *, size_t, uint64_t pts_us, int keyframe);
int ewrtc_media_send_audio(ewrtc_media *, const uint8_t *, size_t, uint64_t pts_us);
int ewrtc_media_receive(ewrtc_media *, const uint8_t *, size_t, bool rtcp);
/* RTCP send errors are returned; AGAIN/BACKPRESSURE may be retried by ticking. */
/* Absolute monotonic milliseconds; UINT64_MAX means no timer. */
uint64_t ewrtc_media_next_deadline(ewrtc_media *);
int ewrtc_media_tick(ewrtc_media *);
int ewrtc_media_get_stats(const ewrtc_media *, ewrtc_media_stats *);

#ifdef __cplusplus
}
#endif
#endif
