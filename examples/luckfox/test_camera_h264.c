#include "camera_h264.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned frames;
static uint64_t last_pts;
static int last_key;
static uint8_t received[256];
static size_t received_size;
static void frame(const uint8_t *data, size_t n, uint64_t pts, int key, void *user) {
    (void)user;
    assert(n <= sizeof(received));
    memcpy(received, data, n); received_size = n;
    frames++; last_pts = pts; last_key = key;
}
static int packet(camera_h264 *h, unsigned seq, uint32_t ts, int marker,
                  const uint8_t *payload, size_t size) {
    uint8_t data[256] = {0x80, 96};
    assert(size + 12 <= sizeof(data));
    data[1] |= marker ? 128 : 0;
    data[2] = seq >> 8; data[3] = seq;
    data[4] = ts >> 24; data[5] = ts >> 16; data[6] = ts >> 8; data[7] = ts;
    memcpy(data + 12, payload, size);
    return camera_h264_packet(h, data, size + 12);
}
int main(void) {
    camera_h264 *h = calloc(1, sizeof(*h)); assert(h);
    h->frame = frame;
    /* STAP-A parameter sets followed by a fragmented IDR. */
    const uint8_t sets[] = {24, 0, 4, 0x67, 0x42, 0xc0, 0x1f, 0, 2, 0x68, 0xaa};
    const uint8_t start[] = {0x7c, 0x85, 0x11, 0x22};
    const uint8_t end[] = {0x7c, 0x45, 0x33, 0x44};
    const uint8_t p[] = {0x41, 0x55};
    const uint8_t idr[] = {0x65, 0x66};
    assert(!packet(h, 1, 0xfffff000, 0, sets, sizeof(sets)));
    assert(!packet(h, 2, 0xfffff000, 0, start, sizeof(start)));
    assert(!packet(h, 3, 0xfffff000, 1, end, sizeof(end)));
    assert(frames == 1 && last_key && last_pts == 0);
    const uint8_t expected_tail[] = {0,0,0,1,0x65,0x11,0x22,0x33,0x44};
    assert(!memcmp(received + received_size - sizeof(expected_tail), expected_tail, sizeof(expected_tail)));
    assert(!packet(h, 4, (uint32_t)(0xfffff000u + 9000u), 1, p, sizeof(p)));
    assert(frames == 2 && !last_key && last_pts == 100000);
    /* A lost FU fragment must never produce a corrupt access unit. */
    assert(!packet(h, 5, 14000, 0, start, sizeof(start)));
    assert(!packet(h, 7, 14000, 1, end, sizeof(end)));
    assert(frames == 2);
    assert(!packet(h, 8, 23000, 1, idr, sizeof(idr)));
    assert(frames == 3 && last_key);
    assert(received[4] == 0x67); /* Cached SPS is present on a later IDR. */
    const uint8_t broken_stap[] = {24, 0, 20, 1};
    assert(packet(h, 9, 32000, 1, broken_stap, sizeof(broken_stap)) < 0);
    assert(frames == 3);
    const uint8_t broken_extension[] = {0x90, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    assert(camera_h264_packet(h, broken_extension, sizeof(broken_extension)) < 0);
    free(h); puts("camera H.264 packetization tests passed");
    return 0;
}
