#include "private.h"
#include <string.h>
static int start_code(const uint8_t *data, size_t size, size_t from, size_t *prefix) {
    for (size_t i = from; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *prefix = 3;
                return (int)i;
            }
            if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
                *prefix = 4;
                return (int)i;
            }
        }
    }
    return -1;
}

int ewrtc_media_validate_video(const uint8_t *data, size_t size, int keyframe) {
    if (!data || !size || size > EWRTC_MAX_FRAME)
        return EWRTC_INVALID;
    size_t prefix;
    int pos = start_code(data, size, 0, &prefix);
    if (pos != 0)
        return EWRTC_INVALID;
    bool idr = false, sps = false, pps = false;
    while (pos >= 0) {
        size_t at = (size_t)pos + prefix;
        if (at >= size)
            return EWRTC_INVALID;
        unsigned type = data[at] & 31;
        if (type == 5)
            idr = true;
        if (type == 7)
            sps = true;
        if (type == 8)
            pps = true;
        size_t next_prefix = 0;
        pos = start_code(data, size, at, &next_prefix);
        if (pos >= 0)
            prefix = next_prefix;
    }
    return (idr == (keyframe != 0) && (!idr || (sps && pps))) ? EWRTC_OK : EWRTC_INVALID;
}

int ewrtc_media_send_video(ewrtc_media *s, const uint8_t *data, size_t size, uint64_t pts_us,
                           int keyframe) {
    if (!s || ewrtc_media_validate_video(data, size, keyframe))
        return EWRTC_INVALID;
    media_state *m = &s->media;
    if (!m->video_started) {
        m->video_pts_base = pts_us;
        m->video_started = true;
    }
    if (pts_us < m->video_pts_base)
        return EWRTC_INVALID;
    uint32_t ts = m->video_ts_base + (uint32_t)(((pts_us - m->video_pts_base) * 90) / 1000);
    size_t prefix;
    int pos = start_code(data, size, 0, &prefix);
    if (pos != 0)
        return EWRTC_INVALID;
    while (pos >= 0) {
        size_t nalu_start = (size_t)pos + prefix;
        size_t next_prefix;
        int next = start_code(data, size, nalu_start, &next_prefix);
        size_t nalu_end = next < 0 ? size : (size_t)next;
        while (nalu_end > nalu_start && data[nalu_end - 1] == 0)
            nalu_end--;
        if (nalu_end <= nalu_start)
            return EWRTC_INVALID;
        const uint8_t *nalu = data + nalu_start;
        size_t nalu_len = nalu_end - nalu_start;
        bool last_nalu = next < 0;
        const size_t max_payload = EWRTC_MTU - 12 - 16 - (s->cfg.rtx_pt >= 0 ? 2 : 0);
        if (nalu_len <= max_payload) {
            int result = media_send_rtp(s, (uint8_t)s->cfg.video_pt, m->video_seq++, ts, m->video_ssrc,
                                  last_nalu, nalu, nalu_len, true, 1);
            if (result)
                return result;
        } else {
            if (nalu_len < 2)
                return EWRTC_INVALID;
            uint8_t payload[EWRTC_MTU];
            payload[0] = (uint8_t)((nalu[0] & 0xe0) | 28);
            size_t offset = 1;
            while (offset < nalu_len) {
                size_t chunk = nalu_len - offset;
                if (chunk > max_payload - 2)
                    chunk = max_payload - 2;
                payload[1] = (uint8_t)((nalu[0] & 0x1f) | (offset == 1 ? 0x80 : 0) |
                                       (offset + chunk == nalu_len ? 0x40 : 0));
                memcpy(payload + 2, nalu + offset, chunk);
                bool marker = last_nalu && offset + chunk == nalu_len;
                int result = media_send_rtp(s, (uint8_t)s->cfg.video_pt, m->video_seq++, ts,
                                      m->video_ssrc, marker, payload, chunk + 2, true, 1);
                if (result)
                    return result;
                offset += chunk;
            }
        }
        pos = next;
        if (next >= 0)
            prefix = next_prefix;
    }
    m->last_video_ts = ts;
    return 0;
}

