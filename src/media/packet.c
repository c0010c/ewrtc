#include "private.h"
#include <string.h>
int media_send_plain(ewrtc_media *s, uint8_t *packet, size_t len, bool rtcp, int kind) {
    return s->cfg.send(s->cfg.user, packet, len, rtcp, kind);
}

int media_send_rtp(ewrtc_media *s, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                    bool marker, const uint8_t *payload, size_t payload_len, bool cache, int kind) {
    if (payload_len + 12 + 16 > EWRTC_MTU)
        return EWRTC_INVALID;
    uint8_t packet[2048];
    ewrtc_rtp_packet r = {pt, marker, seq, ts, ssrc, payload, payload_len};
    size_t packet_size;
    if (ewrtc_rtp_write(&r, packet, sizeof(packet), &packet_size))
        return EWRTC_INVALID;
    int result = cache ? media_add_cache(s, packet, packet_size, seq, ts) : 0;
    if (result)
        return result;
    result = media_send_plain(s, packet, packet_size, false, kind);
    if (result)
        return result;
    if (kind == 1) {
        s->media.video_packets++;
        s->stats.sent_video_packets++;
        s->media.video_octets += (uint32_t)payload_len;
    } else if (kind == 2) {
        s->media.audio_packets++;
        s->stats.sent_audio_packets++;
        s->media.audio_octets += (uint32_t)payload_len;
    }
    return 0;
}

