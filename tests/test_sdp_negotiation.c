#include "sdp/sdp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void *alloc(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p) { (void)ctx; free(p); }
static const char fingerprint[] = "A8:D3:E5:7B:31:D6:76:9E:70:C8:03:6F:BF:EE:53:AC:BF:83:94:6C:2C:FE:0E:51:C7:F0:19:AA:8B:18:50:E9";
int main(void) {
    ewrtc_pal pal = {.memory = {NULL, alloc, NULL, release}};
    ewrtc_sdp_local identity = {"local123", "password-123456789012345", fingerprint};
    ewrtc_sdp_offer offer = {.video_pt=102,.rtx_pt=103,.audio_pt=111,
        .video_mid="0",.audio_mid="1",.h264_profile="42e01f",.nack=true,.pli=true};
    char wire[4096], answer[4096];
    for (int vd=0;vd<4;++vd) for (int ad=0;ad<4;++ad) for (int order=0;order<2;++order) {
        offer.video_direction=vd; offer.audio_direction=ad; offer.audio_first=order;
        ewrtc_sdp_offer parsed, remote;
        assert(!ewrtc_sdp_make_description(&offer,&identity,11,12,13,wire,sizeof(wire)));
        assert(!ewrtc_sdp_parse_offer(&pal,wire,&parsed));
        assert(parsed.video_direction==vd && parsed.audio_direction==ad);
        assert(parsed.audio_first==order && parsed.nack && parsed.pli);
        assert(parsed.video_ssrc == (ewrtc_direction_sends(vd) ? 11u : 0u));
        assert(!ewrtc_sdp_make_answer(&parsed,&identity,21,22,23,answer,sizeof(answer)));
        assert(!ewrtc_sdp_parse_answer(&pal,answer,&remote));
        assert(!ewrtc_sdp_validate_answer(&parsed,&remote));
        assert(ewrtc_sdp_parse_answer(&pal,wire,&remote)); /* actpass forbidden in answer */
    }
    offer.video_direction=EWRTC_SENDONLY; offer.audio_direction=EWRTC_SENDRECV;
    ewrtc_sdp_offer parsed, remote;
    assert(!ewrtc_sdp_make_description(&offer,&identity,11,12,13,wire,sizeof(wire)));
    assert(!ewrtc_sdp_parse_offer(&pal,wire,&parsed));
    assert(!ewrtc_sdp_make_answer(&parsed,&identity,21,22,23,answer,sizeof(answer)));
    assert(!ewrtc_sdp_parse_answer(&pal,answer,&remote));
    ewrtc_sdp_offer bad=remote;
    bad.video_direction=EWRTC_SENDRECV; assert(ewrtc_sdp_validate_answer(&parsed,&bad));
    bad=remote; bad.video_pt++; assert(ewrtc_sdp_validate_answer(&parsed,&bad));
    bad=remote; bad.audio_first=!bad.audio_first; assert(ewrtc_sdp_validate_answer(&parsed,&bad));
    bad=remote; strcpy(bad.video_mid,"other"); assert(ewrtc_sdp_validate_answer(&parsed,&bad));
    bad=remote; strcpy(bad.h264_profile,"42001f"); assert(ewrtc_sdp_validate_answer(&parsed,&bad));
    bad=remote; bad.rtx_pt=-1; assert(!ewrtc_sdp_validate_answer(&parsed,&bad));
    char *at=strstr(answer,"a=setup:passive"); assert(at); memcpy(at,"a=setup:active ",15);
    assert(ewrtc_sdp_parse_answer(&pal,answer,&bad));
    /* Conflicting bundle credentials must not silently override the first track. */
    assert(!ewrtc_sdp_make_answer(&parsed,&identity,21,22,23,answer,sizeof(answer)));
    at=strstr(answer,"a=ice-ufrag:local123"); assert(at);
    at=strstr(at+1,"a=ice-ufrag:local123"); assert(at); at[12]='X';
    assert(ewrtc_sdp_parse_answer(&pal,answer,&bad));
    puts("SDP offer/answer directions, roles, payloads, MID and BUNDLE validation passed");
}
