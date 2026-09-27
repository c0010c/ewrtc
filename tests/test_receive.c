#include "media/media.h"
#include "rtp/rtp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now;
    size_t live, alloc_calls, fail_at;
    unsigned frames, keyframes, nack, pli;
    uint32_t timestamp, report_highest, report_lsr, report_dlsr;
    uint16_t nack_pid, nack_mask;
    uint8_t frame[8192];
    size_t size;
} fixture;
static void *allocate(void *ctx, size_t n) {
    fixture *f = ctx;
    if (++f->alloc_calls == f->fail_at) return NULL;
    void *p = malloc(n); if (p) ++f->live; return p;
}
static void *resize(void *ctx, void *p, size_t n) {
    fixture *f = ctx;
    if (++f->alloc_calls == f->fail_at) return NULL;
    bool fresh = !p;
    void *q = realloc(p, n); if (q && fresh) ++f->live; return q;
}
static void release(void *ctx, void *p) { if (p) { --((fixture *)ctx)->live; free(p); } }
static uint64_t clock_ms(void *ctx) { return ((fixture *)ctx)->now; }
static int random_bytes(void *ctx, void *p, size_t n) { (void)ctx; memset(p, 5, n); return 0; }
static int send_packet(void *ctx, const uint8_t *p, size_t n, bool rtcp, int kind) {
    (void)kind;
    fixture *f = ctx;
    assert(rtcp);
    size_t off = 0; ewrtc_rtcp_packet block;
    while (!ewrtc_rtcp_next(p, n, &off, &block)) {
        if (block.type == 205) {
            ++f->nack;
            f->nack_pid = ewrtc_read_u16(block.bytes + 12);
            f->nack_mask = ewrtc_read_u16(block.bytes + 14);
        }
        if (block.type == 201 && block.count == 1) {
            f->report_highest = ewrtc_read_u32(block.bytes + 16);
            f->report_lsr = ewrtc_read_u32(block.bytes + 24);
            f->report_dlsr = ewrtc_read_u32(block.bytes + 28);
        }
        if (block.type == 206) ++f->pli;
    }
    return 0;
}
static void video(void *ctx, const uint8_t *p, size_t n, uint32_t ts, int key) {
    fixture *f = ctx;
    assert(n <= sizeof(f->frame));
    memcpy(f->frame, p, n); f->size = n; f->timestamp = ts;
    ++f->frames; f->keyframes += key != 0;
}
static ewrtc_media *create(fixture *f) {
    ewrtc_pal pal = {.memory = {f, allocate, resize, release},
        .clock = {f, clock_ms, clock_ms}, .random = {f, random_bytes}};
    ewrtc_media_config c = {.pal = pal, .video_pt = 102, .rtx_pt = 103, .audio_pt = 111,
        .video_ssrc = 7, .audio_ssrc = 8, .rtx_ssrc = 9,
        .remote_video_ssrc = 42, .remote_rtx_ssrc = 43,
        .send = send_packet, .video = video, .user = f, .nack = true, .pli = true};
    ewrtc_media *m; assert(!ewrtc_media_create(&c, &m)); return m;
}
static int packet(ewrtc_media *m, uint16_t seq, uint32_t ts, bool marker,
                  const uint8_t *data, size_t size, bool rtx) {
    uint8_t wire[2048]; size_t n;
    ewrtc_rtp_packet p = {.payload_type = rtx ? 103 : 102, .sequence = seq,
        .timestamp = ts, .marker = marker, .ssrc = rtx ? 43 : 42,
        .payload = data, .payload_size = size};
    assert(!ewrtc_rtp_write(&p, wire, sizeof(wire), &n));
    return ewrtc_media_receive(m, wire, n, false);
}
static void test_reorder_rtx_wrap(void) {
    fixture f = {.now = 1000}; ewrtc_media *m = create(&f);
    assert(ewrtc_media_next_deadline(m) == UINT64_MAX);
    const uint8_t stap[] = {0x78, 0, 2, 0x67, 1, 0, 2, 0x68, 2};
    const uint8_t start[] = {0x7c, 0x85, 3, 4};
    const uint8_t end[] = {0x7c, 0x45, 7, 8};
    const uint8_t rtx[] = {0, 0, 0x7c, 0x05, 5, 6};
    assert(!packet(m, 65534, 9000, false, stap, sizeof(stap), false));
    assert(!packet(m, 65535, 9000, false, start, sizeof(start), false));
    assert(!packet(m, 1, 9000, true, end, sizeof(end), false));
    assert(!f.frames);
    assert(ewrtc_media_next_deadline(m) <= f.now);
    assert(!ewrtc_media_tick(m) && f.nack == 1);
    assert(ewrtc_media_next_deadline(m) == f.now + 30);
    assert(!packet(m, 400, 9000, false, rtx, sizeof(rtx), true));
    const uint8_t expected[] = {0,0,0,1,0x67,1,0,0,0,1,0x68,2,0,0,0,1,0x65,3,4,5,6,7,8};
    assert(f.frames == 1 && f.keyframes == 1 && f.timestamp == 9000);
    assert(f.size == sizeof(expected) && !memcmp(f.frame, expected, sizeof(expected)));
    assert(!packet(m, 1, 9000, true, end, sizeof(end), false) && f.frames == 1);
    ewrtc_media_stats stats; assert(!ewrtc_media_get_stats(m, &stats));
    assert(stats.received_video_packets == 4 && stats.received_rtx_packets == 1);
    ewrtc_media_destroy(m); assert(!f.live);
}
static void test_loss_and_invalid(void) {
    fixture f = {.now = 1000}; ewrtc_media *m = create(&f);
    const uint8_t start[] = {0x7c, 0x81, 1}, end[] = {0x7c, 0x41, 3}, idr[] = {0x65, 7};
    assert(!packet(m, 10, 1, false, start, sizeof(start), false));
    assert(!packet(m, 12, 1, true, end, sizeof(end), false));
    f.now += 151; assert(!ewrtc_media_tick(m));
    assert(!f.frames && f.pli == 1);
    assert(ewrtc_media_next_deadline(m) > f.now);
    assert(!packet(m, 13, 2, true, idr, sizeof(idr), false) && f.frames == 1);
    const uint8_t bad_stap[] = {0x78, 0, 20, 0x65};
    assert(packet(m, 14, 3, true, bad_stap, sizeof(bad_stap), false) == EWRTC_INVALID);
    const uint8_t bad_fu[] = {0x7c, 0xc5, 0};
    assert(packet(m, 15, 4, true, bad_fu, sizeof(bad_fu), false) == EWRTC_INVALID);
    assert(packet(m, 16, 5, true, end, sizeof(end), false) == EWRTC_INVALID);
    assert(f.frames == 1);
    assert(!packet(m, 17, 6, true, idr, sizeof(idr), false) && f.frames == 2);
    /* Lost first packet of a new AU: never emit the remaining NAL as a full AU. */
    assert(!packet(m, 19, 7, true, idr, sizeof(idr), false));
    f.now += 151; assert(!ewrtc_media_tick(m)); assert(f.frames == 2);
    assert(!packet(m, 20, 8, true, idr, sizeof(idr), false) && f.frames == 3);
    ewrtc_media_destroy(m); assert(!f.live);
}
static void test_allocation_and_instances(void) {
    for (unsigned fail = 1; fail <= 2; ++fail) {
        fixture f = {.now = 1000}; ewrtc_media *m = create(&f);
        f.fail_at = f.alloc_calls + fail;
        const uint8_t idr[] = {0x65, 7};
        assert(packet(m, 1, 1, true, idr, sizeof(idr), false) == EWRTC_NOMEM);
        assert(!f.frames); ewrtc_media_destroy(m); assert(!f.live);
    }
    fixture a = {0}, b = {0}; ewrtc_media *ma = create(&a), *mb = create(&b);
    const uint8_t idr[] = {0x65, 7};
    assert(!packet(ma, 1, 100, true, idr, sizeof(idr), false));
    assert(!packet(mb, 1, 200, true, idr, sizeof(idr), false));
    assert(a.timestamp == 100 && b.timestamp == 200);
    ewrtc_media_destroy(ma); ewrtc_media_destroy(mb); assert(!a.live && !b.live);
}
static void test_feedback_and_limits(void) {
    fixture f = {.now = 1000}; ewrtc_media *m = create(&f);
    const uint8_t start[] = {0x7c,0x85,1}, end[] = {0x7c,0x45,3};
    assert(!packet(m,10,9000,false,start,sizeof(start),false));
    assert(!packet(m,14,9000,true,end,sizeof(end),false));
    assert(!ewrtc_media_tick(m));
    assert(f.nack == 1 && f.nack_pid == 11 && f.nack_mask == 3 && f.report_highest == 14);
    uint8_t sr[28];
    assert(!ewrtc_rtcp_write_sr(42,9000,5,100,1700000000000000ULL,sr,sizeof(sr)));
    assert(!ewrtc_media_receive(m,sr,sizeof(sr),true));
    f.now += 1000; assert(!ewrtc_media_tick(m));
    assert(f.report_lsr == ((ewrtc_read_u32(sr+8)<<16)|(ewrtc_read_u32(sr+12)>>16)));
    assert(f.report_dlsr == 65536 && !f.frames);
    ewrtc_media_destroy(m); assert(!f.live);
    f = (fixture){.now=1000}; m=create(&f);
    assert(!packet(m,1,9000,false,start,sizeof(start),false));
    uint8_t fragment[1200]; memset(fragment,1,sizeof(fragment)); fragment[0]=0x7c; fragment[1]=5;
    bool rejected=false;
    for (unsigned seq=2;seq<2000;++seq) {
        int result=packet(m,(uint16_t)seq,9000,false,fragment,sizeof(fragment),false);
        if (result) { assert(result==EWRTC_INVALID); rejected=true; break; }
    }
    assert(rejected && !f.frames);
    ewrtc_media_destroy(m); assert(!f.live);
}
int main(void) {
    test_feedback_and_limits();
    test_reorder_rtx_wrap(); test_loss_and_invalid(); test_allocation_and_instances();
    puts("H264 receive, reorder, RTX, wraparound, loss, malformed payload and allocation tests passed");
}
