#ifndef EWRTC_H
#define EWRTC_H

#include "ewrtc/pal.h"
#include "ewrtc/backends.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ewrtc_session ewrtc_session;
typedef struct ewrtc_context ewrtc_context;
typedef struct {
    ewrtc_pal pal;
    size_t worker_count;       /* 0: 1 */
    size_t max_sessions;       /* 0: 8 */
    size_t queue_limit_bytes;  /* 0: 8 MiB, includes control reservations */
    size_t control_slots;      /* 0: 64 per session */
} ewrtc_context_config;
typedef struct {
    size_t sessions, workers, sdk_threads;
    size_t control_reserved_bytes, queue_bytes, peak_queue_bytes;
    uint64_t backpressure_count, congestion_drops;
    uint64_t max_schedule_wait_ms, max_timer_delay_ms, max_callback_ms, max_turn_ms;
} ewrtc_context_stats;
void ewrtc_context_config_init(ewrtc_context_config *);
ewrtc_result ewrtc_context_create(const ewrtc_context_config *, ewrtc_context **);
ewrtc_result ewrtc_context_get_stats(ewrtc_context *, ewrtc_context_stats *);
/* Fails with STATE while any session handle remains. Forbidden in callbacks. */
ewrtc_result ewrtc_context_destroy(ewrtc_context *);

typedef struct {
    ewrtc_crypto_backend crypto_backend;
    ewrtc_ice_backend ice_backend;
    ewrtc_dtls_backend dtls_backend;
    const char *stun_host;
    uint16_t stun_port;
    const char *turn_host;
    uint16_t turn_port;
    const char *turn_username;
    const char *turn_password;
    int relay_only;                       /* Restrict advertised and selected path to TURN/UDP. */
    size_t send_queue_limit_bytes;        /* 0: 1 MiB */
    size_t retransmit_cache_limit_bytes;  /* 0: 2 MiB */
    uint32_t retransmit_cache_max_age_ms; /* 0: 2000 ms */
    ewrtc_direction video_direction, audio_direction; /* local capabilities */
} ewrtc_session_config;

typedef struct {
    uint64_t sent_video_packets, sent_audio_packets, received_audio_packets;
    uint64_t sent_bytes, received_bytes, nack_requests, rtx_packets;
    uint64_t pli_requests, dropped_packets, backpressure_count;
    size_t send_queue_bytes, retransmit_cache_bytes;
    ewrtc_state state;
    ewrtc_ice_backend ice_backend;
    ewrtc_dtls_backend dtls_backend;
    char local_candidate[256], remote_candidate[256];
    uint64_t received_video_packets, received_video_frames, dropped_video_frames;
    uint64_t sent_nacks, sent_plis, received_rtx_packets;
} ewrtc_stats;

typedef struct {
    void (*on_state)(ewrtc_session *, ewrtc_state, void *user);
    void (*on_local_sdp)(ewrtc_session *, const char *sdp, void *user);
    void (*on_local_candidate)(ewrtc_session *, const char *candidate, void *user);
    void (*on_gathering_done)(ewrtc_session *, void *user);
    void (*on_audio)(ewrtc_session *, const uint8_t *opus, size_t length, uint32_t rtp_timestamp,
                     uint16_t sequence, void *user);
    void (*on_keyframe_request)(ewrtc_session *, void *user);
    void (*on_error)(ewrtc_session *, ewrtc_result, const char *detail, void *user);
    /* Complete Annex-B access unit; timestamp is the remote 90 kHz RTP clock.
     * Borrowed until callback returns. keyframe is nonzero for an IDR AU. */
    void (*on_video)(ewrtc_session *, const uint8_t *annex_b, size_t length,
                     uint32_t rtp_timestamp, int keyframe, void *user);
} ewrtc_callbacks;

/* Sets compiled backend defaults. PAL belongs to the explicit context. */
void ewrtc_session_config_init(ewrtc_session_config *);

/* Callbacks run serially on the assigned context worker; never block them.
 * Data is borrowed. destroy of ANY session/context is forbidden in callbacks. */
ewrtc_result ewrtc_session_create(ewrtc_context *context, const ewrtc_session_config *config,
                                  const ewrtc_callbacks *callbacks, void *user, ewrtc_session **out);
/* Choose exactly one initial negotiation path per session. create_offer emits
 * an offer through on_local_sdp; set_remote_offer emits an answer there.
 * Answers can be submitted after the local offer callback begins. */
ewrtc_result ewrtc_session_create_offer(ewrtc_session *s);
ewrtc_result ewrtc_session_set_remote_answer(ewrtc_session *s, const char *sdp);
ewrtc_result ewrtc_session_set_remote_offer(ewrtc_session *s, const char *sdp);
ewrtc_result ewrtc_session_add_remote_candidate(ewrtc_session *s, const char *candidate);
ewrtc_result ewrtc_session_end_remote_candidates(ewrtc_session *s);
ewrtc_result ewrtc_session_send_video(ewrtc_session *s, const uint8_t *annex_b, size_t length,
                                      uint64_t pts_us, int keyframe);
ewrtc_result ewrtc_session_send_audio(ewrtc_session *s, const uint8_t *opus, size_t length,
                                      uint64_t pts_us);
ewrtc_result ewrtc_session_get_stats(ewrtc_session *s, ewrtc_stats *out);
ewrtc_result ewrtc_session_close(ewrtc_session *s);
/* Must be outside every SDK callback. Caller excludes concurrent handle use.
 * Waits for preparation, producer joins and final CLOSED callback; synchronous
 * DNS may delay completion indefinitely. No callback occurs after return. */
ewrtc_result ewrtc_session_destroy(ewrtc_session *s);

#ifdef __cplusplus
}
#endif
#endif
