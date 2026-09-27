#include "common/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if HAVE_PAL
#include "pal/pal.h"
typedef struct {
    size_t attempts, fail_at, live;
    uint64_t mono, utc;
    bool random_fail;
    unsigned random_calls;
} fake;
static void *allocate(void *ctx, size_t n) {
    fake *f = ctx;
    if (++f->attempts == f->fail_at)
        return NULL;
    void *p = malloc(n);
    if (p)
        ++f->live;
    return p;
}
static void *resize(void *ctx, void *p, size_t n) {
    fake *f = ctx;
    if (++f->attempts == f->fail_at)
        return NULL;
    bool fresh = p == NULL;
    void *q = realloc(p, n);
    if (q && fresh)
        ++f->live;
    return q;
}
static void release(void *ctx, void *p) {
    fake *f = ctx;
    if (p) {
        assert(f->live);
        --f->live;
        free(p);
    }
}
static uint64_t mono(void *ctx) {
    return ((fake *)ctx)->mono;
}
static uint64_t utc(void *ctx) {
    return ((fake *)ctx)->utc;
}
static int random_bytes(void *ctx, void *p, size_t n) {
    fake *f = ctx;
    ++f->random_calls;
    if (f->random_fail)
        return EWRTC_SECURITY;
    memset(p, 0x12, n);
    return 0;
}
static ewrtc_pal make_pal(fake *f) {
    ewrtc_pal p = {.memory = {f, allocate, resize, release},
                   .clock = {f, mono, utc},
                   .random = {f, random_bytes}};
    return p;
}
#endif
static void test_common(void) {
    ewrtc_address a;
    char text[16];
    assert(!ewrtc_address_parse("192.0.2.123", 12345, &a));
    assert(a.ipv4 == 0xc000027bu && a.port == 12345);
    assert(!ewrtc_address_format(&a, text, sizeof(text)) && !strcmp(text, "192.0.2.123"));
    assert(ewrtc_address_parse("1.2.3.256", 0, &a) == EWRTC_INVALID);
    assert(ewrtc_address_parse("1.2.3", 0, &a) == EWRTC_INVALID);
    assert(ewrtc_address_parse("1.2.3.4x", 0, &a) == EWRTC_INVALID);
    assert(!ewrtc_utc_format(0, text) && !strcmp(text, "19700101000000"));
    assert(!ewrtc_utc_format(951782400, text) && !strcmp(text, "20000229000000"));
    assert(!ewrtc_ascii_casecmp("H264", "h264"));
}
#if HAVE_RTP
#include "rtp/rtp.h"
static void test_rtp(void) {
    uint8_t data[64], payload[] = {1, 2, 3};
    size_t n;
    ewrtc_rtp_packet r = {111, true, 65535, 0xffffffff, 123, payload, sizeof(payload)}, out;
    assert(!ewrtc_rtp_write(&r, data, sizeof(data), &n));
    assert(!ewrtc_rtp_parse(data, n, &out));
    assert(out.sequence == 65535 && out.timestamp == 0xffffffff && out.marker &&
           out.payload_size == 3);
    assert(!memcmp(out.payload, payload, 3));
    for (size_t i = 0; i < 12; ++i)
        assert(ewrtc_rtp_parse(data, i, &out) == EWRTC_INVALID);
    data[0] |= 0x20;
    data[n++] = 1;
    assert(!ewrtc_rtp_parse(data, n, &out) && out.payload_size == 3);
    data[n - 1] = 30;
    assert(ewrtc_rtp_parse(data, n, &out) == EWRTC_INVALID);
    data[0] = 0x90;
    assert(ewrtc_rtp_parse(data, 12, &out) == EWRTC_INVALID);
    data[12] = 0;
    data[13] = 0;
    data[14] = 0xff;
    data[15] = 0xff;
    assert(ewrtc_rtp_parse(data, 16, &out) == EWRTC_INVALID);
    assert(!ewrtc_rtcp_write_sr(1, 2, 3, 4, 1000000, data, sizeof(data)));
    assert(ewrtc_read_u32(data + 8) == 2208988801u);
    size_t off = 0;
    ewrtc_rtcp_packet b;
    assert(!ewrtc_rtcp_next(data, 28, &off, &b) && b.type == 200 && b.size == 28);
    assert(ewrtc_rtcp_next(data, 28, &off, &b) == EWRTC_AGAIN);
    off = 0;
    assert(ewrtc_rtcp_next(data, 27, &off, &b) == EWRTC_INVALID);
    assert(!ewrtc_rtcp_write_feedback(206, 1, 2, 3, NULL, 0, data, 64, &n));
    off = 0;
    assert(!ewrtc_rtcp_next(data, n, &off, &b) && b.count == 1 && b.size == 12);
}
#endif
#if HAVE_SDP
#include "sdp/sdp.h"
static const char offer[] =
    "v=0\r\na=group:BUNDLE 0 1\r\na=ice-options:trickle\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 "
    "103\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\na=setup:actpass\r\n"
    "a=ice-ufrag:test\r\na=ice-pwd:password\r\n"
    "a=fingerprint:sha-256 "
    "A8:D3:E5:7B:31:D6:76:9E:70:C8:03:6F:BF:EE:53:AC:BF:83:94:6C:2C:FE:0E:51:C7:F0:19:AA:8B:18:50:"
    "E9\r\n"
    "a=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF "
    "111\r\na=mid:1\r\na=sendrecv\r\na=rtcp-mux\r\na=setup:actpass\r\na=rtpmap:111 "
    "opus/48000/2\r\n";
static void test_sdp(void) {
    fake f = {0};
    ewrtc_pal p = {.memory = {&f, allocate, NULL, release}};
    assert(!ewrtc_pal_validate(&p, EWRTC_PAL_ALLOCATOR));
    assert(ewrtc_pal_validate(&p, EWRTC_PAL_RESIZE) == EWRTC_INVALID);
    assert(ewrtc_pal_validate(&p, 1u << 31) == EWRTC_INVALID);
    ewrtc_sdp_offer o;
    assert(!ewrtc_sdp_parse_offer(&p, offer, &o));
    assert(!f.live);
    ewrtc_sdp_local local = {"local", "local-password", o.fingerprint};
    char answer[4096];
    assert(!ewrtc_sdp_make_answer(&o, &local, 1, 2, 3, answer, sizeof(answer)));
    assert(strstr(answer, "a=ice-ufrag:local\r\n") && strstr(answer, "a=setup:passive"));
    assert(ewrtc_sdp_make_answer(&o, &local, 1, 2, 3, answer, 8));
    f.fail_at = f.attempts + 1;
    assert(ewrtc_sdp_parse_offer(&p, offer, &o));
    assert(!f.live);
    assert(ewrtc_sdp_parse_offer(&p, "invalid", &o));
    assert(!f.live);
}
#endif
#if HAVE_MEDIA
#include "media/media.h"
typedef struct {
    uint8_t packets[16][2048];
    size_t sizes[16];
    bool control[16];
    size_t count;
    unsigned audio, keyframes;
    int send_error;
} sink;
static int capture(void *ctx, const uint8_t *p, size_t n, bool rtcp, int kind) {
    (void)kind;
    sink *s = ctx;
    if (s->send_error) return s->send_error;
    assert(s->count < 16 && n <= 2048);
    memcpy(s->packets[s->count], p, n);
    s->sizes[s->count] = n;
    s->control[s->count++] = rtcp;
    return 0;
}
static void audio(void *ctx, const uint8_t *p, size_t n, uint32_t ts, uint16_t seq) {
    (void)p;
    (void)ts;
    (void)seq;
    assert(n == 3);
    ++((sink *)ctx)->audio;
}
static void keyframe(void *ctx) {
    ++((sink *)ctx)->keyframes;
}
static ewrtc_media_config media_config(ewrtc_pal p, sink *s) {
    return (ewrtc_media_config){.pal = p,
                                .video_pt = 102,
                                .rtx_pt = 103,
                                .audio_pt = 111,
                                .video_ssrc = 1,
                                .rtx_ssrc = 2,
                                .audio_ssrc = 3,
                                .cache_limit_bytes = 8192,
                                .cache_max_age_ms = 2000,
                                .send = capture,
                                .audio = audio,
                                .keyframe = keyframe,
                                .user = s};
}
static void test_media_send_errors(void) {
    fake f = {.utc = 1700000000000000ULL};
    sink output = {0};
    ewrtc_media_config c = media_config(make_pal(&f), &output);
    ewrtc_media *m;
    assert(!ewrtc_media_create(&c, &m));
    const uint8_t opus[] = {0xf8, 0xff, 0xfe};
    const int errors[] = {EWRTC_AGAIN, EWRTC_BACKPRESSURE, EWRTC_IO};
    for (unsigned i = 0; i < 3; ++i) {
        output.send_error = errors[i];
        assert(ewrtc_media_send_audio(m, opus, sizeof(opus), 0) == errors[i]);
        ewrtc_media_stats stats;
        assert(!ewrtc_media_get_stats(m, &stats) && !stats.sent_audio_packets);
    }
    output.send_error = 0;
    assert(!ewrtc_media_send_audio(m, opus, sizeof(opus), 0));
    f.mono = 1000;
    output.send_error = EWRTC_AGAIN;
    assert(ewrtc_media_tick(m) == EWRTC_AGAIN);
    output.send_error = 0;
    assert(!ewrtc_media_tick(m) && output.control[1]);
    const uint8_t video[] = {0, 0, 0, 1, 0x41, 1, 2, 3};
    assert(!ewrtc_media_send_video(m, video, sizeof(video), 0, 0));
    uint8_t feedback[16], fci[4] = {0}; size_t size;
    ewrtc_write_u16(fci, ewrtc_read_u16(output.packets[2] + 2));
    assert(!ewrtc_rtcp_write_feedback(205, 1, 7, 1, fci, sizeof(fci), feedback, sizeof(feedback), &size));
    output.send_error = EWRTC_BACKPRESSURE;
    assert(ewrtc_media_receive(m, feedback, size, true) == EWRTC_BACKPRESSURE);
    output.send_error = 0;
    assert(!ewrtc_media_receive(m, feedback, size, true));
    ewrtc_media_stats stats;
    assert(!ewrtc_media_get_stats(m, &stats) && stats.rtx_packets == 1);
    ewrtc_media_destroy(m);
    assert(!f.live);
}
static void test_media_instances(void) {
    fake first = {.utc = 5000000}, second = {.utc = 7000000};
    sink one = {0}, two = {0};
    ewrtc_media_config a = media_config(make_pal(&first), &one);
    ewrtc_media_config b = media_config(make_pal(&second), &two);
    ewrtc_media *m1, *m2;
    assert(!ewrtc_media_create(&a, &m1));
    assert(!ewrtc_media_create(&b, &m2));
    /* Caller-side table changes must not alter either constructed instance. */
    memset(&a.pal, 0, sizeof(a.pal));
    memset(&b.pal, 0, sizeof(b.pal));
    uint8_t opus[] = {0xf8, 0xff, 0xfe};
    assert(!ewrtc_media_send_audio(m1, opus, 3, 0));
    assert(!ewrtc_media_send_audio(m2, opus, 3, 0));
    first.mono = 1001;
    ewrtc_media_tick(m1);
    ewrtc_media_tick(m2);
    assert(one.count == 2 && two.count == 1);
    assert(ewrtc_read_u32(one.packets[1] + 8) == 2208988805u);
    second.mono = 1001;
    ewrtc_media_tick(m2);
    assert(two.count == 2 && ewrtc_read_u32(two.packets[1] + 8) == 2208988807u);
    assert(first.random_calls == 1 && second.random_calls == 1);
    ewrtc_media_destroy(m1);
    assert(!first.live && second.live);
    ewrtc_media_destroy(m2);
    assert(!second.live);
}
static void test_media(void) {
    fake f = {.mono = 100, .utc = 1000000};
    ewrtc_pal p = make_pal(&f);
    sink s = {0};
    ewrtc_media_config c = media_config(p, &s);
    ewrtc_media *m;
    assert(!ewrtc_media_create(&c, &m));
    uint8_t frame[3005] = {0, 0, 0, 1, 0x41};
    memset(frame + 5, 1, 3000);
    assert(!ewrtc_media_send_video(m, frame, sizeof(frame), 1000000, 0));
    assert(s.count == 3);
    ewrtc_rtp_packet a, b;
    assert(!ewrtc_rtp_parse(s.packets[0], s.sizes[0], &a) && a.payload_type == 102 && !a.marker);
    assert((a.payload[0] & 31) == 28 && (a.payload[1] & 128));
    assert(!ewrtc_rtp_parse(s.packets[2], s.sizes[2], &b) && b.marker && (b.payload[1] & 64));
    uint8_t nack[16], fci[4];
    size_t n;
    ewrtc_write_u16(fci, a.sequence);
    ewrtc_write_u16(fci + 2, 0);
    assert(!ewrtc_rtcp_write_feedback(205, 1, 7, 1, fci, 4, nack, sizeof(nack), &n));
    assert(!ewrtc_media_receive(m, nack, n, true));
    assert(s.count == 4);
    assert(!ewrtc_rtp_parse(s.packets[3], s.sizes[3], &b) && b.payload_type == 103 && b.ssrc == 2);
    assert(ewrtc_read_u16(b.payload) == a.sequence && b.timestamp == a.timestamp);
    assert(!memcmp(b.payload + 2, a.payload, a.payload_size));
    uint8_t pli[12];
    assert(!ewrtc_rtcp_write_feedback(206, 1, 7, 1, NULL, 0, pli, 12, &n));
    assert(!ewrtc_media_receive(m, pli, n, true) && s.keyframes == 1);
    uint8_t fir[20], firfci[8] = {0};
    ewrtc_write_u32(firfci, 1);
    assert(!ewrtc_rtcp_write_feedback(206, 4, 7, 0, firfci, 8, fir, 20, &n));
    assert(!ewrtc_media_receive(m, fir, n, true) && s.keyframes == 2);
    uint8_t opus[] = {0xf8, 0xff, 0xfe};
    assert(!ewrtc_media_send_audio(m, opus, 3, 1000000));
    assert(!ewrtc_media_receive(m, s.packets[4], s.sizes[4], false) && s.audio == 1);
    ewrtc_media_stats stats;
    assert(!ewrtc_media_get_stats(m, &stats));
    assert(stats.rtx_packets == 1 && stats.nack_requests == 1 && stats.pli_requests == 2 &&
           stats.retransmit_cache_bytes > 0);
    size_t bytes = stats.retransmit_cache_bytes;
    f.utc = 999999999999ULL;
    ewrtc_media_tick(m);
    ewrtc_media_get_stats(m, &stats);
    assert(stats.retransmit_cache_bytes == bytes); /* wall clock jump cannot expire cache */
    f.mono += 2001;
    s.count = 0;
    ewrtc_media_tick(m);
    ewrtc_media_get_stats(m, &stats);
    assert(stats.retransmit_cache_bytes == 0 && s.count == 2 && s.control[0]);
    assert(ewrtc_read_u32(s.packets[0] + 8) == (uint32_t)(f.utc / 1000000) + 2208988800u);
    size_t before = s.count;
    assert(!ewrtc_media_receive(m, nack, 16, true) && s.count == before);
    ewrtc_media_destroy(m);
    assert(!f.live);
    fake g = {.mono = 7};
    sink t = {0};
    ewrtc_pal q = make_pal(&g);
    c = media_config(q, &t);
    assert(!ewrtc_media_create(&c, &m));
    assert(g.random_calls == 1 && f.random_calls == 1);
    ewrtc_media_destroy(m);
    assert(!g.live);
    for (size_t at = 1; at <= 3; ++at) {
        f = (fake){.fail_at = at};
        p = make_pal(&f);
        s = (sink){0};
        c = media_config(p, &s);
        int r = ewrtc_media_create(&c, &m);
        if (!r) {
            assert(ewrtc_media_send_video(m, frame, sizeof(frame), 0, 0));
            ewrtc_media_destroy(m);
        }
        assert(!f.live);
    }
    f = (fake){.random_fail = true};
    c = media_config(make_pal(&f), &s);
    assert(ewrtc_media_create(&c, &m) == EWRTC_SECURITY && !f.live);
    f = (fake){0};
    c = media_config(make_pal(&f), &s);
    c.cache_limit_bytes = 32;
    assert(!ewrtc_media_create(&c, &m));
    assert(ewrtc_media_send_video(m, frame, sizeof(frame), 0, 0));
    ewrtc_media_destroy(m);
    assert(!f.live);
}
static void test_media_without_rtx(void) {
    fake f = {0}; sink output = {0};
    ewrtc_media_config c = media_config(make_pal(&f), &output);
    c.rtx_pt = -1;
    ewrtc_media *m;
    assert(!ewrtc_media_create(&c, &m));
    const uint8_t frame[] = {0,0,0,1,0x41,1,2,3};
    assert(!ewrtc_media_send_video(m,frame,sizeof(frame),0,0));
    uint8_t nack[16] = {0x81,205,0,3};
    ewrtc_write_u32(nack+8,c.video_ssrc);
    ewrtc_write_u16(nack+12,ewrtc_read_u16(output.packets[0]+2));
    assert(!ewrtc_media_receive(m,nack,sizeof(nack),true));
    assert(output.count==2 && output.sizes[0]==output.sizes[1]);
    assert(!memcmp(output.packets[0],output.packets[1],output.sizes[0]));
    ewrtc_media_destroy(m); assert(!f.live);
}
#endif
#if HAVE_SRTP
#include "srtp/srtp.h"
static void test_srtp(void) {
    fake f = {0};
    ewrtc_pal p = make_pal(&f);
    uint8_t keys[60];
    for (unsigned i = 0; i < 60; ++i)
        keys[i] = (uint8_t)i;
    ewrtc_srtp *server, *client;
    assert(!ewrtc_srtp_create(&p, keys, true, &server));
    assert(!ewrtc_srtp_create(&p, keys, false, &client));
    uint8_t data[128] = {0x80, 111, 0, 1, 0, 0, 0, 1, 0, 0, 0, 7, 0xf8, 0xff, 0xfe}, copy[128];
    size_t size = 15;
    assert(!ewrtc_srtp_protect(server, false, data, sizeof(data), &size) && size == 25);
    memcpy(copy, data, size);
    size_t good = size;
    copy[size - 1] ^= 1;
    assert(ewrtc_srtp_unprotect(client, false, copy, &size) == EWRTC_SECURITY);
    size = good;
    assert(!ewrtc_srtp_unprotect(client, false, data, &size) && size == 15 && data[12] == 0xf8);
    memset(data, 0, 28);
    data[0] = 128;
    data[1] = 200;
    data[3] = 6;
    data[7] = 7;
    size = 28;
    assert(!ewrtc_srtp_protect(client, true, data, sizeof(data), &size));
    assert(!ewrtc_srtp_unprotect(server, true, data, &size) && size == 28);
    ewrtc_srtp_destroy(server);
    ewrtc_srtp_destroy(client);
    assert(!f.live);
    f.fail_at = f.attempts + 1;
    assert(ewrtc_srtp_create(&p, keys, true, &server) == EWRTC_NOMEM && !f.live);
}
#endif
#if HAVE_CRYPTO
#include "crypto/crypto.h"
static void test_crypto(void) {
    const uint8_t md5_expected[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                                      0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    const uint8_t hmac_expected[20] = {0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
                                       0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00};
    for (int backend = 0; backend < 2; ++backend) {
        if (!ewrtc_crypto_available((ewrtc_crypto_backend)backend))
            continue;
        uint8_t digest[20], key[20];
        memset(key, 0x0b, 20);
        assert(!ewrtc_crypto_md5((ewrtc_crypto_backend)backend, "abc", 3, digest));
        assert(!memcmp(digest, md5_expected, 16));
        assert(
            !ewrtc_crypto_hmac_sha1((ewrtc_crypto_backend)backend, "Hi There", 8, key, 20, digest));
        assert(!memcmp(digest, hmac_expected, 20));
    }
    uint8_t out[20];
    assert(ewrtc_crypto_md5((ewrtc_crypto_backend)99, "abc", 3, out) == EWRTC_UNSUPPORTED);
}
#endif
#if HAVE_STUN
#include "stun/stun.h"
static void test_stun_inputs(void) {
    uint8_t tx[12] = {0};
    ewrtc_stun_writer w;
    ewrtc_stun_packet packet;
    assert(ewrtc_stun_begin(NULL, 1, tx) == EWRTC_INVALID);
    assert(ewrtc_stun_begin(&w, 1, NULL) == EWRTC_INVALID);
    assert(!ewrtc_stun_begin(&w, STUN_BINDING_REQUEST, tx));
    assert(ewrtc_stun_add(&w, ATTR_USERNAME, NULL, 2) == EWRTC_INVALID);
    assert(ewrtc_stun_add(&w, ATTR_USERNAME, tx, SIZE_MAX) == EWRTC_INVALID);
    assert(ewrtc_stun_parse(w.bytes, 19, &packet) == EWRTC_INVALID);
    ewrtc_address a = {0xc0000201, 65535}, b;
    assert(!ewrtc_stun_add_xor_address(&w, ATTR_XOR_MAPPED_ADDRESS, &a));
    assert(!ewrtc_stun_add_fingerprint(&w));
    assert(!ewrtc_stun_parse(w.bytes, w.len, &packet));
    assert(!ewrtc_stun_decode_xor_address(&packet,
                                          ewrtc_stun_find(&packet, ATTR_XOR_MAPPED_ADDRESS), &b));
    assert(a.ipv4 == b.ipv4 && a.port == b.port);
    w.bytes[w.len - 1] ^= 1;
    assert(ewrtc_stun_parse(w.bytes, w.len, &packet));
}
#endif
#if HAVE_DTLS
#include "dtls/dtls.h"
static int discard_dtls(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    (void)data;
    (void)size;
    return 0;
}
static void test_dtls_lifecycle(void) {
    const char *fingerprint = "A8:D3:E5:7B:31:D6:76:9E:70:C8:03:6F:BF:EE:53:AC:BF:83:94:6C:2C:FE:"
                              "0E:51:C7:F0:19:AA:8B:18:50:E9";
    for (int backend = 0; backend < 2; ++backend) {
        fake f = {.utc = 1700000000000000ULL};
        ewrtc_dtls_config c = {.pal = make_pal(&f),
                               .backend = (ewrtc_dtls_backend)backend,
                               .remote_fingerprint = fingerprint,
                               .send = discard_dtls};
        ewrtc_dtls *d;
        int r = ewrtc_dtls_create(&c, &d);
        if (r == EWRTC_UNSUPPORTED)
            continue;
        assert(!r);
        uint8_t keys[60];
        assert(ewrtc_dtls_export_keys(d, keys) == EWRTC_STATE);
        assert(strlen(ewrtc_dtls_fingerprint(d)) == 95);
        assert(!ewrtc_dtls_start(d));
        assert(ewrtc_dtls_start(d) == EWRTC_STATE);
        assert(!ewrtc_dtls_tick(d));
        ewrtc_dtls_destroy(d);
        assert(!f.live);
        f.fail_at = f.attempts + 1;
        assert(ewrtc_dtls_create(&c, &d) == EWRTC_NOMEM && !d && !f.live);
    }
}
#endif
int main(void) {
    test_common();
#if HAVE_CRYPTO
    test_crypto();
#endif
#if HAVE_STUN
    test_stun_inputs();
#endif
#if HAVE_DTLS
    test_dtls_lifecycle();
#endif
#if HAVE_RTP
    test_rtp();
#endif
#if HAVE_SDP
    test_sdp();
#endif
#if HAVE_MEDIA
    test_media();
    test_media_instances();
    test_media_send_errors();
    test_media_without_rtx();
#endif
#if HAVE_SRTP
    test_srtp();
#endif
    puts("component isolation, protocol, allocation, clock and media tests passed");
    return 0;
}
