#define _POSIX_C_SOURCE 200809L
#include "ewrtc.h"
#include "ewrtc/platform/linux.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if EWRTC_WITH_OPENSSL
#include <openssl/err.h>
#endif

typedef struct peer {
    ewrtc_session *session;
    struct peer *remote;
    bool offerer, passive_offer, delay_answer, embedded;
    atomic_int connected, failed, videos, audios, sdp_ready, candidates;
    char answer[16384];
} peer;
static void pause_ms(unsigned ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void state(ewrtc_session *s, ewrtc_state state, void *ctx) {
    (void)s; peer *p = ctx;
    if (state == EWRTC_CONNECTED) atomic_store(&p->connected, 1);
    if (state == EWRTC_FAILED) atomic_store(&p->failed, 1);
}
static void error(ewrtc_session *s, ewrtc_result r, const char *detail, void *ctx) {
    (void)s; peer *p = ctx;
    fprintf(stderr, "%s error %d: %s\n", p->offerer ? "offerer" : "answerer", r, detail);
}
static void dispatch_sdp(ewrtc_session *s, const char *text, void *ctx) {
    (void)s; peer *p = ctx;
    if (p->offerer) {
        if (p->passive_offer) {
            char copy[4096]; snprintf(copy, sizeof(copy), "%s", text);
            char *at; while ((at = strstr(copy, "setup:actpass"))) memcpy(at, "setup:passive", 13);
            assert(!ewrtc_session_set_remote_offer(p->remote->session, copy));
        } else assert(!ewrtc_session_set_remote_offer(p->remote->session, text));
    } else if (p->delay_answer) {
        snprintf(p->answer, sizeof(p->answer), "%s", text);
        atomic_store(&p->sdp_ready, 1);
    } else assert(!ewrtc_session_set_remote_answer(p->remote->session, text));
}
static void sdp(ewrtc_session *s, const char *text, void *ctx) {
    peer *p = ctx;
    if (p->embedded) snprintf(p->answer, sizeof(p->answer), "%s", text);
    else dispatch_sdp(s, text, ctx);
#if EWRTC_WITH_OPENSSL
    /* An application's unrelated OpenSSL failure must not poison another
     * session's SSL_get_error on the shared worker. */
    ERR_put_error(ERR_LIB_USER, 0, 1, __FILE__, __LINE__);
#endif
}
static void candidate(ewrtc_session *s, const char *text, void *ctx) {
    (void)s; peer *p = ctx;
    if (p->embedded) {
        if (!strncmp(text, "a=", 2)) text += 2;
        size_t used = strlen(p->answer);
        assert(used + strlen(text) + 5 < sizeof(p->answer));
        snprintf(p->answer + used, sizeof(p->answer) - used, "a=%s\r\n", text);
    } else assert(!ewrtc_session_add_remote_candidate(p->remote->session, text));
    atomic_fetch_add(&p->candidates, 1);
}
static void done(ewrtc_session *s, void *ctx) {
    (void)s; peer *p = ctx;
    if (p->embedded) {
        strcat(p->answer, "a=end-of-candidates\r\n");
        if (p->offerer) assert(!ewrtc_session_set_remote_offer(p->remote->session, p->answer));
        else atomic_store(&p->sdp_ready, 1);
    } else assert(!ewrtc_session_end_remote_candidates(p->remote->session));
}
static void video(ewrtc_session *s, const uint8_t *p, size_t n, uint32_t ts, int key, void *ctx) {
    (void)s; (void)ts;
    assert(n == 25 && key && p[4] == 0x67 && p[10] == 0x68 && p[16] == 0x65);
    atomic_fetch_add(&((peer *)ctx)->videos, 1);
}
static void audio(ewrtc_session *s, const uint8_t *p, size_t n, uint32_t ts, uint16_t seq, void *ctx) {
    (void)s; (void)ts; (void)seq;
    assert(n == 3 && p[0] == 0xf8); atomic_fetch_add(&((peer *)ctx)->audios, 1);
}
static void run_pair(int ice_a, int tls_a, int ice_b, int tls_b, bool passive, bool one_way, bool embedded, int invalid) {
    printf("connecting ICE/DTLS %d/%d -> %d/%d passive=%d invalid=%d\n", ice_a, tls_a, ice_b, tls_b, passive, invalid);
    peer a = {.offerer = true, .passive_offer = passive, .embedded = embedded},
         b = {.delay_answer = true, .embedded = embedded};
    a.remote = &b; b.remote = &a;
    ewrtc_callbacks cb = {.on_state = state, .on_local_sdp = sdp, .on_local_candidate = candidate,
        .on_gathering_done = done, .on_error = error, .on_video = video, .on_audio = audio};
    ewrtc_session_config c; ewrtc_session_config_init(&c); ewrtc_context_config cc; ewrtc_context_config_init(&cc);
    cc.pal = *ewrtc_pal_linux();
    ewrtc_context *context;
    assert(!ewrtc_context_create(&cc, &context));
    c.ice_backend = ice_a; c.dtls_backend = tls_a; c.crypto_backend = tls_a;
    if (one_way) c.video_direction = c.audio_direction = EWRTC_RECVONLY;
    assert(!ewrtc_session_create(context, &c, &cb, &a, &a.session));
    c.ice_backend = ice_b; c.dtls_backend = tls_b; c.crypto_backend = tls_b;
    if (one_way) c.video_direction = c.audio_direction = EWRTC_SENDONLY;
    assert(!ewrtc_session_create(context, &c, &cb, &b, &b.session));
    assert(ewrtc_session_set_remote_answer(a.session, "v=0") == EWRTC_STATE);
    assert(!ewrtc_session_create_offer(a.session));
    assert(ewrtc_session_create_offer(a.session) == EWRTC_STATE);
    assert(ewrtc_session_set_remote_offer(a.session, "v=0") == EWRTC_STATE);
    for (int i = 0; i < 500 && !atomic_load(&b.sdp_ready); ++i) pause_ms(10);
    assert(atomic_load(&b.sdp_ready));
    pause_ms(100); /* candidates intentionally arrive before the answer */
    assert(atomic_load(&b.candidates));
    if (invalid == 1) {
        char *at;
        while ((at = strstr(b.answer, "setup:passive"))) memcpy(at, "setup:actpass", 13);
    } else if (invalid == 2) {
        char *at = b.answer;
        while ((at = strstr(at, "a=fingerprint:sha-256 "))) {
            at[22] = at[22] == '0' ? '1' : '0';
            at += 23;
        }
    }
    assert(!ewrtc_session_set_remote_answer(a.session, b.answer));
    if (invalid) {
        for (unsigned i = 0; i < 500 && !atomic_load(&a.failed); ++i) pause_ms(10);
        assert(atomic_load(&a.failed) && !atomic_load(&a.connected));
        assert(!ewrtc_session_destroy(a.session)); assert(!ewrtc_session_destroy(b.session));
    assert(!ewrtc_context_destroy(context));
        printf("invalid answer/fingerprint case %d rejected\n", invalid);
        return;
    }
    assert(ewrtc_session_set_remote_answer(a.session, b.answer) == EWRTC_STATE);
    for (int i = 0; i < 800 && (!atomic_load(&a.connected) || !atomic_load(&b.connected)); ++i) {
        assert(!atomic_load(&a.failed) && !atomic_load(&b.failed)); pause_ms(10);
    }
    if (!atomic_load(&a.connected) || !atomic_load(&b.connected)) {
        ewrtc_stats sa, sb; ewrtc_session_get_stats(a.session, &sa); ewrtc_session_get_stats(b.session, &sb);
        fprintf(stderr, "connect timeout: a=%d (%s -> %s), b=%d (%s -> %s)\n", sa.state,
            sa.local_candidate,sa.remote_candidate,sb.state,sb.local_candidate,sb.remote_candidate);

    }
    assert(atomic_load(&a.connected) && atomic_load(&b.connected));
    const uint8_t frame[] = {0,0,0,1,0x67,1,0,0,0,1,0x68,2,0,0,0,1,0x65,3,4,5,6,7,8,9,10};
    const uint8_t opus[] = {0xf8, 0xff, 0xfe};
    assert(ewrtc_session_send_video(a.session, frame, sizeof(frame), 0, 1) == (one_way ? EWRTC_STATE : EWRTC_OK));
    assert(!ewrtc_session_send_video(b.session, frame, sizeof(frame), 0, 1));
    assert(ewrtc_session_send_audio(a.session, opus, sizeof(opus), 0) == (one_way ? EWRTC_STATE : EWRTC_OK));
    assert(!ewrtc_session_send_audio(b.session, opus, sizeof(opus), 0));
    for (int i = 0; i < 300 && (!atomic_load(&a.videos) || !atomic_load(&a.audios) ||
         (!one_way && (!atomic_load(&b.videos) || !atomic_load(&b.audios)))); ++i) pause_ms(10);
    assert(atomic_load(&a.videos) && atomic_load(&a.audios));
    assert(one_way ? (!atomic_load(&b.videos) && !atomic_load(&b.audios)) :
                    (atomic_load(&b.videos) && atomic_load(&b.audios)));
    assert(!ewrtc_session_destroy(a.session)); assert(!ewrtc_session_destroy(b.session));
    assert(!ewrtc_context_destroy(context));
    printf("pair ICE/DTLS %d/%d -> %d/%d, offer passive=%d passed\n", ice_a, tls_a, ice_b, tls_b, passive);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int ia = 0; ia < 2; ++ia) for (int ta = 0; ta < 2; ++ta)
    for (int ib = 0; ib < 2; ++ib) for (int tb = 0; tb < 2; ++tb) {
        if ((!EWRTC_WITH_NATIVE_ICE && (!ia || !ib)) || (!EWRTC_WITH_LIBJUICE && (ia || ib)) ||
            (!EWRTC_WITH_OPENSSL && (!ta || !tb)) || (!EWRTC_WITH_MBEDTLS && (ta || tb))) continue;
        run_pair(ia, ta, ib, tb, false, false, false, 0);
        run_pair(ia, ta, ib, tb, true, false, false, 0);
    }
    int ice = EWRTC_WITH_NATIVE_ICE ? 0 : 1, tls = EWRTC_WITH_OPENSSL ? 0 : 1;
    run_pair(ice, tls, ice, tls, false, true, true, 0);
    run_pair(ice, tls, ice, tls, false, false, false, 1);
    run_pair(ice, tls, ice, tls, false, false, false, 2);

}
