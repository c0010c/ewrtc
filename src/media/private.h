#ifndef EWRTC_MEDIA_PRIVATE_H
#define EWRTC_MEDIA_PRIVATE_H
#include "media/media.h"
#include "rtp/rtp.h"
typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint64_t at_ms;
    uint8_t *rtp;
    size_t length;
} retransmit_entry;

typedef struct {
    uint16_t video_seq, rtx_seq, audio_seq;
    uint32_t video_ssrc, rtx_ssrc, audio_ssrc;
    uint32_t video_ts_base, audio_ts_base;
    uint64_t video_pts_base, audio_pts_base;
    bool video_started, audio_started;
    retransmit_entry *cache;
    size_t cache_count, cache_capacity, cache_bytes, cache_limit;
    uint32_t cache_age_ms;
    uint32_t last_video_ts, last_audio_ts;
    uint32_t video_packets, audio_packets, video_octets, audio_octets;
    uint64_t last_rtcp_ms;
} media_state;

#define VIDEO_REORDER_PACKETS 128u
typedef struct {
    uint16_t sequence;
    uint32_t timestamp;
    bool marker;
    size_t size;
    uint8_t data[];
} video_packet;
typedef struct {
    video_packet *packets[VIDEO_REORDER_PACKETS];
    uint8_t *frame;
    size_t size, capacity;
    uint32_t ssrc, rtx_ssrc, timestamp, last_timestamp, highest, base_sequence;
    uint32_t received, expected_prior, received_prior, lsr, transit;
    int64_t jitter_q4;
    bool have_transit;
    uint64_t gap_at, nack_at, pli_at, frame_at, sr_at;
    uint16_t next;
    uint8_t fu_header;
    bool started, frame_active, broken, fu_active, keyframe, pli_pending;
    bool have_last_timestamp, gap_pending, have_sr;
} video_receiver;

struct ewrtc_media {
    ewrtc_media_config cfg;
    media_state media;
    video_receiver receiver;
    ewrtc_media_stats stats;
    uint64_t retry_at;
};

int media_receive_video(ewrtc_media *, const ewrtc_rtp_packet *, bool rtx);
uint64_t media_receiver_deadline(ewrtc_media *);
int media_receiver_tick(ewrtc_media *);
int media_receiver_report(ewrtc_media *, uint8_t *out, size_t *size);
void media_receiver_destroy(ewrtc_media *);
void media_evict_cache(ewrtc_media *s);
int media_add_cache(ewrtc_media *s, const uint8_t *rtp, size_t len, uint16_t seq,
                     uint32_t timestamp);
int media_send_plain(ewrtc_media *s, uint8_t *packet, size_t len, bool rtcp, int kind);
int media_send_rtp(ewrtc_media *s, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                    bool marker, const uint8_t *payload, size_t payload_len, bool cache, int kind);
int media_send_rtx(ewrtc_media *s, uint16_t original_seq);
int media_receive_rtcp(ewrtc_media *s, const uint8_t *packet, size_t len);
int media_receive_rtp(ewrtc_media *s, const uint8_t *packet, size_t len);
#endif
