#include "private.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

void dtls_adapter_init(dtls_adapter *base, const ewrtc_dtls_config *config, const dtls_ops *ops) {
    base->pal = config->pal;
    base->ops = ops;
    base->send = config->send;
    base->send_user = config->user;
    if (config->remote_fingerprint)
        memcpy(base->remote_fingerprint, config->remote_fingerprint, DTLS_FINGERPRINT_SIZE);
}

int dtls_format_fingerprint(const uint8_t digest[DTLS_SHA256_SIZE], char *out, size_t size) {
    if (size < DTLS_FINGERPRINT_SIZE)
        return EWRTC_SECURITY;
    size_t offset = 0;
    for (unsigned i = 0; i < DTLS_SHA256_SIZE; ++i) {
        int written = snprintf(out + offset, size - offset, "%s%02X", i ? ":" : "", digest[i]);
        if (written < 0 || (size_t)written >= size - offset)
            return EWRTC_SECURITY;
        offset += (size_t)written;
    }
    return EWRTC_OK;
}

int ewrtc_dtls_create(const ewrtc_dtls_config *c, ewrtc_dtls **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!c || !c->send ||
        (c->remote_fingerprint && strlen(c->remote_fingerprint) != DTLS_FINGERPRINT_SIZE - 1) ||
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
    if (!s || !fingerprint || strlen(fingerprint) != DTLS_FINGERPRINT_SIZE - 1)
        return EWRTC_INVALID;
    if (s->started)
        return EWRTC_STATE;
    for (unsigned i = 0; i < DTLS_FINGERPRINT_SIZE - 1; ++i)
        if ((i + 1) % 3 == 0 ? fingerprint[i] != ':' : !isxdigit((unsigned char)fingerprint[i]))
            return EWRTC_INVALID;
    int result = s->ops->set_client(s, client);
    if (!result)
        memcpy(s->remote_fingerprint, fingerprint, DTLS_FINGERPRINT_SIZE);
    return result;
}
static int drive_result(ewrtc_dtls *s, int result) {
    s->retry_at = result == EWRTC_AGAIN || result == EWRTC_BACKPRESSURE
                    ? ewrtc_now_ms(&s->pal) + 20 : 0;
    return result;
}
uint64_t ewrtc_dtls_next_deadline(ewrtc_dtls *s) {
    if (!s || !s->started || s->connected)
        return UINT64_MAX;
    if (s->retry_at)
        return s->retry_at;
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
    if (!s || !d || !n)
        return EWRTC_INVALID;
    if (!s->started)
        return EWRTC_STATE;
    return drive_result(s, s->ops->receive(s, d, n));
}
int ewrtc_dtls_tick(ewrtc_dtls *s) {
    if (!s)
        return EWRTC_INVALID;
    if (!s->started)
        return EWRTC_STATE;
    return drive_result(s, s->ops->tick(s));
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
