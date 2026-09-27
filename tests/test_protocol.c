#include "ewrtc.h"
#include "ewrtc/platform/linux.h"
#include "sdp/sdp.h"
#if EWRTC_WITH_NATIVE_ICE
#include "stun/stun.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static atomic_int error_count;
static void error_cb(ewrtc_session *session, ewrtc_result result, const char *detail, void *user) {
    (void)session;
    (void)result;
    (void)detail;
    (void)user;
    atomic_fetch_add(&error_count, 1);
}

static const char chrome_offer[] =
    "v=0\r\na=group:BUNDLE 0 1\r\na=ice-options:trickle\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 103 108 109\r\n"
    "a=mid:0\r\na=recvonly\r\na=rtcp-mux\r\na=setup:actpass\r\n"
    "a=ice-ufrag:remote12\r\n"
    "a=ice-pwd:remote-password-1234567890\r\n"
    "a=fingerprint:sha-256 A8:D3:E5:7B:31:D6:76:9E:70:C8:03:6F:BF:EE:53:AC:"
    "BF:83:94:6C:2C:FE:0E:51:C7:F0:19:AA:8B:18:50:E9\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=fmtp:102 packetization-mode=1;profile-level-id=42001f\r\n"
    "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
    "a=rtpmap:108 H264/90000\r\n"
    "a=fmtp:108 packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:109 rtx/90000\r\na=fmtp:109 apt=108\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=mid:1\r\na=sendrecv\r\na=rtcp-mux\r\na=setup:actpass\r\n"
    "a=rtpmap:111 opus/48000/2\r\n";

static void test_sdp(void) {
    ewrtc_sdp_offer offer;
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), chrome_offer, &offer) == 0);
    assert(offer.video_pt == 108 && offer.rtx_pt == 109);
    assert(offer.audio_pt == 111 && !strcmp(offer.h264_profile, "42e01f"));
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), "v=0\r\nm=video 9 RTP/AVP 96\r\n", &offer) !=
           0);
    char invalid[sizeof(chrome_offer)];
    strcpy(invalid, chrome_offer);
    char *field = strstr(invalid, "a=rtcp-mux");
    assert(field);
    memcpy(field, "a=rtcp-bad", strlen("a=rtcp-bad"));
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), invalid, &offer) != 0);
    strcpy(invalid, chrome_offer);
    field = strstr(invalid, "a=fingerprint:sha-256 ");
    assert(field);
    field[strlen("a=fingerprint:sha-256 ")] = 'Z';
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), invalid, &offer) != 0);
    strcpy(invalid, chrome_offer);
    field = strstr(invalid, "UDP/TLS/RTP/SAVPF");
    assert(field);
    field[0] = 'X';
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), invalid, &offer) != 0);
    strcpy(invalid, chrome_offer);
    field = strstr(invalid, "opus/48000/2");
    assert(field);
    field[5] = '8';
    assert(ewrtc_sdp_parse_offer(ewrtc_pal_linux(), invalid, &offer) != 0);
}

#if EWRTC_WITH_NATIVE_ICE
static void test_stun(ewrtc_crypto_backend backend) {
    uint8_t tx[12] = {1, 2, 3}, key[] = "secret";
    ewrtc_stun_writer writer;
    assert(ewrtc_stun_begin(&writer, STUN_BINDING_REQUEST, tx) == 0);
    assert(ewrtc_stun_add(&writer, ATTR_USERNAME, "a:b", 3) == 0);
    assert(ewrtc_stun_add_integrity(&writer, key, 6, backend) == 0);
    assert(ewrtc_stun_add_fingerprint(&writer) == 0);
    ewrtc_stun_packet packet;
    assert(ewrtc_stun_parse(writer.bytes, writer.len, &packet) == 0);
    assert(ewrtc_stun_verify_integrity(&packet, key, 6, backend) == 0);
    assert(ewrtc_stun_verify_integrity(&packet, (const uint8_t *)"wrong", 5, backend) != 0);
    writer.bytes[writer.len - 1] ^= 1;
    assert(ewrtc_stun_parse(writer.bytes, writer.len, &packet) != 0);

    ewrtc_address addr;
    assert(ewrtc_address_parse("192.0.2.123", 54321, &addr) == 0);
    assert(ewrtc_stun_begin(&writer, STUN_BINDING_SUCCESS, tx) == 0);
    assert(ewrtc_stun_add_xor_address(&writer, ATTR_XOR_MAPPED_ADDRESS, &addr) == 0);
    assert(ewrtc_stun_parse(writer.bytes, writer.len, &packet) == 0);
    ewrtc_address decoded;
    assert(ewrtc_stun_decode_xor_address(&packet, ewrtc_stun_find(&packet, ATTR_XOR_MAPPED_ADDRESS),
                                         &decoded) == 0);
    assert(decoded.port == addr.port && decoded.ipv4 == addr.ipv4);
}
#endif

static void test_lifecycle(ewrtc_ice_backend ice, ewrtc_dtls_backend dtls) {
    ewrtc_session_config config;
    ewrtc_session_config_init(&config);
    ewrtc_context_config cc; ewrtc_context_config_init(&cc);
    cc.pal = *ewrtc_pal_linux();
    ewrtc_context *context;
    assert(!ewrtc_context_create(&cc, &context));
    config.ice_backend = ice;
    config.dtls_backend = dtls;
    config.crypto_backend =
        dtls == EWRTC_DTLS_OPENSSL ? EWRTC_CRYPTO_OPENSSL : EWRTC_CRYPTO_MBEDTLS;
    ewrtc_callbacks callbacks = {.on_error = error_cb};
    for (int i = 0; i < 100; ++i) {
        ewrtc_session *session = NULL;
        assert(ewrtc_session_create(context, &config, &callbacks, NULL, &session) == EWRTC_OK);
        ewrtc_stats stats;
        assert(ewrtc_session_get_stats(session, &stats) == EWRTC_OK);
        assert(stats.state == EWRTC_NEW);
        assert(ewrtc_session_add_remote_candidate(
                   session, "candidate:1 1 UDP 1 127.0.0.1 1234 typ host") == EWRTC_STATE);
        assert(ewrtc_session_end_remote_candidates(session) == EWRTC_STATE);
        assert(ewrtc_session_send_audio(session, (const uint8_t *)"x", 1, 0) == EWRTC_STATE);
        uint8_t oversized_audio[1200] = {0};
        assert(ewrtc_session_send_audio(session, oversized_audio, sizeof(oversized_audio), 0) ==
               EWRTC_INVALID);
        const uint8_t missing_sps[] = {0, 0, 1, 0x65, 1};
        assert(ewrtc_session_send_video(session, missing_sps, sizeof(missing_sps), 0, 1) ==
               EWRTC_INVALID);
        assert(ewrtc_session_destroy(session) == EWRTC_OK);
    }
    config.send_queue_limit_bytes = 1;
    ewrtc_session *session = NULL;
    assert(ewrtc_session_create(context, &config, &callbacks, NULL, &session) == EWRTC_OK);
    assert(ewrtc_session_set_remote_offer(session, chrome_offer) == EWRTC_BACKPRESSURE);
    assert(ewrtc_session_destroy(session) == EWRTC_OK);

    config.send_queue_limit_bytes = 0;
    assert(ewrtc_session_create(context, &config, &callbacks, NULL, &session) == EWRTC_OK);
    assert(ewrtc_session_set_remote_offer(session, "invalid offer") == EWRTC_OK);
    ewrtc_stats stats;
    for (int i = 0; i < 100; ++i) {
        assert(ewrtc_session_get_stats(session, &stats) == EWRTC_OK);
        if (stats.state == EWRTC_FAILED || stats.state == EWRTC_CLOSED)
            break;
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    assert((stats.state == EWRTC_FAILED || stats.state == EWRTC_CLOSED) && atomic_load(&error_count) > 0);
    assert(ewrtc_session_destroy(session) == EWRTC_OK);
    assert(!ewrtc_context_destroy(context));
}

int main(void) {
    test_sdp();
#if EWRTC_WITH_NATIVE_ICE
#if EWRTC_WITH_OPENSSL
    test_stun(EWRTC_CRYPTO_OPENSSL);
#endif
#if EWRTC_WITH_MBEDTLS
    test_stun(EWRTC_CRYPTO_MBEDTLS);
#endif
#endif
#if EWRTC_WITH_NATIVE_ICE && EWRTC_WITH_OPENSSL
    test_lifecycle(EWRTC_ICE_NATIVE, EWRTC_DTLS_OPENSSL);
#elif EWRTC_WITH_LIBJUICE && EWRTC_WITH_OPENSSL
    test_lifecycle(EWRTC_ICE_LIBJUICE, EWRTC_DTLS_OPENSSL);
#elif EWRTC_WITH_NATIVE_ICE && EWRTC_WITH_MBEDTLS
    test_lifecycle(EWRTC_ICE_NATIVE, EWRTC_DTLS_MBEDTLS);
#else
    test_lifecycle(EWRTC_ICE_LIBJUICE, EWRTC_DTLS_MBEDTLS);
#endif
    puts("protocol, STUN, and 100 lifecycle tests passed");
    return 0;
}
