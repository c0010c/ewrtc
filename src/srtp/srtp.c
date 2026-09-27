#include "srtp/srtp.h"
#include <srtp2/srtp.h>
#include <stdatomic.h>
#include <limits.h>
#include <string.h>

struct ewrtc_srtp {
    ewrtc_pal pal;
    srtp_t in, out;
};

static atomic_flag init_guard = ATOMIC_FLAG_INIT;
static bool initialized;
static int init_status = EWRTC_SECURITY;

static int initialize(void) {
    while (atomic_flag_test_and_set_explicit(&init_guard, memory_order_acquire)) {
    }
    if (!initialized) {
        init_status = srtp_init() == srtp_err_status_ok ? 0 : EWRTC_SECURITY;
        initialized = true;
    }
    atomic_flag_clear_explicit(&init_guard, memory_order_release);
    return init_status;
}

int ewrtc_srtp_create(const ewrtc_pal *pal, const uint8_t keys[60], bool server, ewrtc_srtp **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!keys || ewrtc_pal_validate(pal, EWRTC_PAL_ALLOCATOR))
        return EWRTC_INVALID;
    if (initialize())
        return EWRTC_SECURITY;
    ewrtc_srtp *s = ewrtc_zalloc(pal, sizeof(*s));
    if (!s)
        return EWRTC_NOMEM;
    s->pal = *pal;
    uint8_t client[30], host[30];
    memcpy(client, keys, 16);
    memcpy(client + 16, keys + 32, 14);
    memcpy(host, keys + 16, 16);
    memcpy(host + 16, keys + 46, 14);
    srtp_policy_t policy = {0};
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
    policy.ssrc.type = ssrc_any_inbound;
    policy.key = server ? client : host;
    policy.window_size = 128;
    if (srtp_create(&s->in, &policy) != srtp_err_status_ok)
        goto fail;
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = server ? host : client;
    policy.allow_repeat_tx = 1;
    if (srtp_create(&s->out, &policy) != srtp_err_status_ok)
        goto fail;
    memset(client, 0, sizeof(client));
    memset(host, 0, sizeof(host));
    *out = s;
    return 0;
fail:
    memset(client, 0, sizeof(client));
    memset(host, 0, sizeof(host));
    ewrtc_srtp_destroy(s);
    return EWRTC_SECURITY;
}

void ewrtc_srtp_destroy(ewrtc_srtp *s) {
    if (!s)
        return;
    if (s->in)
        srtp_dealloc(s->in);
    if (s->out)
        srtp_dealloc(s->out);
    ewrtc_free(&s->pal, s);
}

int ewrtc_srtp_protect(ewrtc_srtp *s, bool rtcp, uint8_t *data, size_t cap, size_t *size) {
    if (!s || !data || !size || *size < (rtcp ? 8u : 12u) || *size > INT_MAX - 32 || cap < *size ||
        cap - *size < 32)
        return EWRTC_INVALID;
    int n = (int)*size;
    srtp_err_status_t r =
        rtcp ? srtp_protect_rtcp(s->out, data, &n) : srtp_protect(s->out, data, &n);
    if (r != srtp_err_status_ok)
        return EWRTC_SECURITY;
    *size = (size_t)n;
    return 0;
}

int ewrtc_srtp_unprotect(ewrtc_srtp *s, bool rtcp, uint8_t *data, size_t *size) {
    if (!s || !data || !size || *size < 22u || *size > INT_MAX)
        return EWRTC_INVALID;
    int n = (int)*size;
    srtp_err_status_t r =
        rtcp ? srtp_unprotect_rtcp(s->in, data, &n) : srtp_unprotect(s->in, data, &n);
    if (r != srtp_err_status_ok)
        return EWRTC_SECURITY;
    *size = (size_t)n;
    return 0;
}
