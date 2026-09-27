#ifndef EWRTC_SDP_H
#define EWRTC_SDP_H
#include "pal/pal.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { EWRTC_SETUP_ACTPASS = 0, EWRTC_SETUP_ACTIVE, EWRTC_SETUP_PASSIVE } ewrtc_setup;
typedef struct {
    int video_pt, rtx_pt, audio_pt;
    char video_mid[32], audio_mid[32];
    char h264_profile[7];
    char ice_ufrag[256], ice_pwd[256], fingerprint[128];
    bool video, audio, bundle, rtcp_mux, trickle, audio_first;
    ewrtc_direction video_direction, audio_direction;
    ewrtc_setup setup;
    uint32_t video_ssrc, rtx_ssrc;
    bool nack, pli;
} ewrtc_sdp_offer;

typedef struct {
    const char *ufrag, *pwd, *fingerprint;
} ewrtc_sdp_local;
/* Same supported two-track H264/Opus subset for offers and answers. */
int ewrtc_sdp_parse_answer(const ewrtc_pal *, const char *, ewrtc_sdp_offer *);
int ewrtc_sdp_validate_answer(const ewrtc_sdp_offer *offer, const ewrtc_sdp_offer *answer);
/* make_description uses local directions and setup from description. */
int ewrtc_sdp_make_description(const ewrtc_sdp_offer *, const ewrtc_sdp_local *,
                               uint32_t, uint32_t, uint32_t, char *, size_t);
int ewrtc_sdp_parse_offer(const ewrtc_pal *, const char *, ewrtc_sdp_offer *);
int ewrtc_sdp_make_answer(const ewrtc_sdp_offer *, const ewrtc_sdp_local *, uint32_t video_ssrc,
                          uint32_t rtx_ssrc, uint32_t audio_ssrc, char *, size_t);

#ifdef __cplusplus
}
#endif
#endif
