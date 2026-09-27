#include "camera_h264.h"
#include <string.h>
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3];
}
static int append(camera_h264 *h, const uint8_t *p, size_t n) {
    if (n > sizeof(h->au) - h->size) { h->broken = true; return -1; }
    memcpy(h->au + h->size, p, n); h->size += n; return 0;
}
static int nal(camera_h264 *h, const uint8_t *p, size_t n) {
    static const uint8_t start[] = {0, 0, 0, 1};
    if (!n) return -1;
    unsigned type = p[0] & 31;
    if (type == 7 || type == 8) {
        if (n > sizeof(h->sps)) return -1;
        if (type == 7) { memcpy(h->sps, p, n); h->sps_size = n; }
        else { memcpy(h->pps, p, n); h->pps_size = n; }
    }
    if (type == 5 && !h->idr) {
        /* Every IDR delivered to ewrtc includes the current parameter sets. */
        if (!h->sps_size || !h->pps_size) { h->broken = true; return -1; }
        if (append(h, start, 4) || append(h, h->sps, h->sps_size) ||
            append(h, start, 4) || append(h, h->pps, h->pps_size)) return -1;
        h->idr = true;
    }
    if (type == 1 || type == 5) h->vcl = true;
    return append(h, start, 4) || append(h, p, n) ? -1 : 0;
}
static void reset_au(camera_h264 *h, uint32_t timestamp) {
    h->timestamp = timestamp;
    h->size = 0; h->broken = h->fragmented = h->idr = h->vcl = false;
}
int camera_h264_packet(camera_h264 *h, const uint8_t *packet, size_t n) {
    if (n < 12 || (packet[0] >> 6) != 2) return -1;
    size_t offset = 12 + 4 * (packet[0] & 15);
    if (offset > n) return -1;
    if (packet[0] & 16) {
        if (n - offset < 4) return -1;
        offset += 4 + 4 * be16(packet + offset + 2);
        if (offset > n) return -1;
    }
    if (packet[0] & 32) {
        size_t padding = packet[n - 1];
        if (!padding || padding > n - offset) return -1;
        n -= padding;
    }
    if (offset >= n) return -1;
    uint16_t sequence = be16(packet + 2);
    uint32_t timestamp = be32(packet + 4);
    if (!h->initialized || timestamp != h->timestamp) reset_au(h, timestamp);
    else if (sequence != h->next_sequence) h->broken = true;
    h->initialized = true; h->next_sequence = (uint16_t)(sequence + 1);
    const uint8_t *p = packet + offset; n -= offset;
    unsigned type = p[0] & 31;
    int result = 0;
    if (type > 0 && type < 24) {
        if (h->fragmented) h->broken = true;
        result = nal(h, p, n);
    } else if (type == 24) {
        size_t i = 1;
        while (i + 2 <= n && !result) {
            size_t length = be16(p + i); i += 2;
            if (!length || length > n - i) { result = -1; break; }
            result = nal(h, p + i, length); i += length;
        }
        if (i != n) result = -1;
    } else if (type == 28 && n >= 3) {
        if (p[1] & 128) {
            if (h->fragmented) h->broken = true;
            uint8_t header = (p[0] & 224) | (p[1] & 31);
            result = nal(h, &header, 1); h->fragmented = true;
        } else if (!h->fragmented) result = -1;
        if (!result) result = append(h, p + 2, n - 2);
        if (p[1] & 64) h->fragmented = false;
    } else result = -1;
    if (result) h->broken = true;
    if (packet[1] & 128) {
        if (!h->broken && !h->fragmented && h->vcl && h->size) {
            if (h->clock_set) h->ticks += (uint32_t)(timestamp - h->previous_timestamp);
            h->previous_timestamp = timestamp; h->clock_set = true;
            h->frame(h->au, h->size, h->ticks * 1000000 / 90000, h->idr, h->user);
        }
        reset_au(h, timestamp);
    }
    return result;
}
