#include "private.h"
#include <string.h>
#include <ctype.h>
int ewrtc_dtls_create(const ewrtc_dtls_config *c, ewrtc_dtls **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!c || !c->send || (c->remote_fingerprint && strlen(c->remote_fingerprint) != 95) ||
        ewrtc_pal_validate(&c->pal, EWRTC_PAL_ALLOCATOR | EWRTC_PAL_UTC | EWRTC_PAL_MONOTONIC))
        return EWRTC_INVALID;
    int error = EWRTC_SECURITY;
    if (c->backend == EWRTC_DTLS_OPENSSL) {
#if EWRTC_WITH_OPENSSL
        *out = dtls_openssl_create(c, &error);
#else
        return EWRTC_UNSUPPORTED;
#endif
    } else if (c->backend == EWRTC_DTLS_MBEDTLS) {
#if EWRTC_WITH_MBEDTLS
        *out = dtls_mbedtls_create(c, &error);
#else
        return EWRTC_UNSUPPORTED;
#endif
    } else
        return EWRTC_INVALID;
    return *out ? 0 : error;
}
int ewrtc_dtls_set_peer(ewrtc_dtls *s, const char *fingerprint, bool client) {
    if (!s || !fingerprint || strlen(fingerprint) != 95)
        return EWRTC_INVALID;
    if (s->started)
        return EWRTC_STATE;
    for (unsigned i = 0; i < 95; ++i)
        if ((i + 1) % 3 == 0 ? fingerprint[i] != ':' : !isxdigit((unsigned char)fingerprint[i]))
            return EWRTC_INVALID;
    int result = s->ops->set_client(s, client);
    if (!result)
        memcpy(s->remote_fingerprint, fingerprint, 96);
    return result;
}
static int drive_result(ewrtc_dtls *s, int result) {
    s->retry_at = result == EWRTC_AGAIN || result == EWRTC_BACKPRESSURE
                    ? ewrtc_now_ms(&s->pal) + 20 : 0;
    return result;
}
uint64_t ewrtc_dtls_next_deadline(ewrtc_dtls *s) {
    if (!s || !s->started || s->connected) return UINT64_MAX;
    if (s->retry_at) return s->retry_at;
    return s->ops->deadline(s);
}
int ewrtc_dtls_start(ewrtc_dtls *s) {
    if (!s)
        return EWRTC_INVALID;
    if (s->started || !s->remote_fingerprint[0])
        return EWRTC_STATE;
    s->started = true;
    return drive_result(s, s->ops->start(s));
}
int ewrtc_dtls_receive(ewrtc_dtls *s, const uint8_t *d, size_t n) {
    return !s || !d || !n ? EWRTC_INVALID
           : !s->started  ? EWRTC_STATE
                          : drive_result(s, s->ops->receive(s, d, n));
}
int ewrtc_dtls_tick(ewrtc_dtls *s) {
    return !s ? EWRTC_INVALID : !s->started ? EWRTC_STATE : drive_result(s, s->ops->tick(s));
}
const char *ewrtc_dtls_fingerprint(const ewrtc_dtls *s) {
    return s ? s->fingerprint : NULL;
}
int ewrtc_dtls_connected(const ewrtc_dtls *s) {
    return s && s->connected;
}
int ewrtc_dtls_export_keys(const ewrtc_dtls *s, uint8_t out[60]) {
    if (!s || !out)
        return EWRTC_INVALID;
    if (!s->connected)
        return EWRTC_STATE;
    memcpy(out, s->key_material, 60);
    return 0;
}
void ewrtc_dtls_destroy(ewrtc_dtls *s) {
    if (s)
        s->ops->destroy(s);
}
