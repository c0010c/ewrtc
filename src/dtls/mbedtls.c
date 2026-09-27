#include "private.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    dtls_adapter base;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_cookie_ctx cookie;
    const uint8_t *incoming;
    size_t incoming_size;
    uint64_t timer_intermediate, timer_final;
    uint8_t hello_body[8192], hello_seen[8192], hello_packet[8217];
    size_t hello_size, hello_received;
    uint16_t hello_sequence, hello_delivered_sequence;
    bool hello_active, hello_delivered;
    bool key_exported;
    int send_error;
} mbed_dtls;

static const mbedtls_ssl_srtp_profile profiles[] = {MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
                                                    MBEDTLS_TLS_SRTP_UNSET};
static size_t be24(const uint8_t *p) {
    return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2];
}
static void put24(uint8_t *p, size_t value) {
    p[0] = (uint8_t)(value >> 16);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)value;
}
static void export_keys(void *ctx, mbedtls_ssl_key_export_type type, const unsigned char *secret,
                        size_t secret_len, const unsigned char client_random[32],
                        const unsigned char server_random[32], mbedtls_tls_prf_types prf) {
    mbed_dtls *d = ctx;
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET)
        return;
    unsigned char seed[64];
    memcpy(seed, client_random, 32);
    memcpy(seed + 32, server_random, 32);
    d->key_exported =
        mbedtls_ssl_tls_prf(prf, secret, secret_len, "EXTRACTOR-dtls_srtp", seed, sizeof(seed),
                            d->base.key_material, sizeof(d->base.key_material)) == 0;
}

static int send_cb(void *ctx, const unsigned char *buf, size_t len) {
    mbed_dtls *d = ctx;
    d->send_error = d->base.send(d->base.send_user, buf, len);
    if (d->send_error == EWRTC_AGAIN || d->send_error == EWRTC_BACKPRESSURE)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return d->send_error ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)len;
}
static int recv_cb(void *ctx, unsigned char *buf, size_t len) {
    mbed_dtls *d = ctx;
    if (!d->incoming)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (len < d->incoming_size)
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(buf, d->incoming, d->incoming_size);
    int result = (int)d->incoming_size;
    d->incoming = NULL;
    d->incoming_size = 0;
    return result;
}
static void timer_set(void *ctx, uint32_t intermediate, uint32_t final) {
    mbed_dtls *d = ctx;
    uint64_t now = ewrtc_now_ms(&d->base.pal);
    d->timer_intermediate = intermediate ? now + intermediate : 0;
    d->timer_final = final ? now + final : 0;
}
static int timer_get(void *ctx) {
    mbed_dtls *d = ctx;
    uint64_t now = ewrtc_now_ms(&d->base.pal);
    if (!d->timer_final)
        return -1; /* Mbed TLS timer sentinel, not an ewrtc_result. */
    if (now >= d->timer_final)
        return 2;
    if (d->timer_intermediate && now >= d->timer_intermediate)
        return 1;
    return 0;
}
static int verify_cb(void *ctx, mbedtls_x509_crt *cert, int depth, uint32_t *flags) {
    (void)ctx;
    (void)cert;
    (void)depth;
    *flags = 0; /* Match the SDP fingerprint after handshake. */
    return 0;
}
static int fingerprint(const mbedtls_x509_crt *cert, char *out, size_t size) {
    uint8_t digest[32];
    if (size < 96 || mbedtls_sha256(cert->raw.p, cert->raw.len, digest, 0))
        return EWRTC_SECURITY;
    size_t off = 0;
    for (size_t i = 0; i < sizeof(digest); ++i) {
        int n = snprintf(out + off, size - off, "%s%02X", i ? ":" : "", digest[i]);
        if (n < 0 || (size_t)n >= size - off)
            return EWRTC_SECURITY;
        off += (size_t)n;
    }
    return 0;
}

static int make_certificate(mbed_dtls *d) {
    if (mbedtls_pk_setup(&d->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(d->key),
                            mbedtls_ctr_drbg_random, &d->rng))
        return EWRTC_SECURITY;
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    mbedtls_x509write_crt_set_subject_key(&writer, &d->key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &d->key);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    uint64_t now = ewrtc_utc_us(&d->base.pal) / 1000000;
    char from[15], to[15];
    if (now < 86400 || ewrtc_utc_format(now - 86400, from) ||
        ewrtc_utc_format(now + 2 * 365 * 86400, to)) {
        mbedtls_x509write_crt_free(&writer);
        return EWRTC_SECURITY;
    }
    uint8_t serial[16];
    if (mbedtls_ctr_drbg_random(&d->rng, serial, sizeof(serial)) ||
        mbedtls_x509write_crt_set_serial_raw(&writer, serial, sizeof(serial)) ||
        mbedtls_x509write_crt_set_subject_name(&writer, "CN=ewrtc") ||
        mbedtls_x509write_crt_set_issuer_name(&writer, "CN=ewrtc") ||
        mbedtls_x509write_crt_set_validity(&writer, from, to)) {
        mbedtls_x509write_crt_free(&writer);
        return EWRTC_SECURITY;
    }
    uint8_t der[2048];
    int len =
        mbedtls_x509write_crt_der(&writer, der, sizeof(der), mbedtls_ctr_drbg_random, &d->rng);
    mbedtls_x509write_crt_free(&writer);
    if (len <= 0 || mbedtls_x509_crt_parse_der(&d->cert, der + sizeof(der) - len, (size_t)len) ||
        fingerprint(&d->cert, d->base.fingerprint, sizeof(d->base.fingerprint)))
        return EWRTC_SECURITY;
    return 0;
}

static int finish(mbed_dtls *d) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&d->ssl);
    char actual[128];
    if (!peer || fingerprint(peer, actual, sizeof(actual)) ||
        ewrtc_ascii_casecmp(actual, d->base.remote_fingerprint)) {
        ewrtc_log(&d->base.pal, 3, "Mbed TLS peer fingerprint mismatch or missing");
        return EWRTC_SECURITY;
    }
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&d->ssl, &info);
    if (info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80) {
        ewrtc_log(&d->base.pal, 3, "Mbed TLS did not negotiate SRTP AES128 SHA1 80");
        return EWRTC_SECURITY;
    }
    if (!d->key_exported)
        return EWRTC_SECURITY;
    d->base.connected = true;
    return 0;
}
static int advance(mbed_dtls *d) {
    d->send_error = 0;
    int ret = mbedtls_ssl_handshake(&d->ssl);
    if (d->send_error)
        return d->send_error;
    if (ret == 0)
        return finish(d);
    if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        if (mbedtls_ssl_session_reset(&d->ssl) ||
            mbedtls_ssl_set_client_transport_id(&d->ssl, (const unsigned char *)"ewrtc", 5))
            return EWRTC_SECURITY;
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    char detail[256];
    mbedtls_strerror(ret, detail, sizeof(detail));
    ewrtc_log(&d->base.pal, 3, detail);
    return EWRTC_SECURITY;
}
static int start(dtls_adapter *base) {
    return advance((mbed_dtls *)base);
}
static int receive(dtls_adapter *base, const uint8_t *data, size_t size) {
    mbed_dtls *d = (mbed_dtls *)base;
    if (d->incoming)
        return EWRTC_SECURITY;
    /* Mbed TLS 3.6 rejects fragmented DTLS ClientHello messages. Chrome
     * fragments its large ClientHello at the path MTU, so reassemble just
     * that handshake message and feed a complete record to Mbed TLS. */
    if (size >= 25 && data[0] == 22 && data[13] == 1) {
        size_t record_len = ((size_t)data[11] << 8) | data[12];
        size_t whole = be24(data + 14), offset = be24(data + 19);
        size_t fragment = be24(data + 22);
        uint16_t sequence = ((uint16_t)data[17] << 8) | data[18];
        if (record_len + 13 != size || record_len != fragment + 12 ||
            whole > sizeof(d->hello_body) || offset > whole || fragment > whole - offset)
            return EWRTC_SECURITY;
        if (d->hello_delivered && sequence == d->hello_delivered_sequence)
            return 0;
        if (offset || fragment != whole) {
            if (!d->hello_active || d->hello_sequence != sequence || d->hello_size != whole) {
                memset(d->hello_seen, 0, whole);
                d->hello_size = whole;
                d->hello_received = 0;
                d->hello_sequence = sequence;
                d->hello_active = true;
                memcpy(d->hello_packet, data, 25);
            }
            memcpy(d->hello_body + offset, data + 25, fragment);
            for (size_t i = offset; i < offset + fragment; ++i)
                if (!d->hello_seen[i]) {
                    d->hello_seen[i] = 1;
                    d->hello_received++;
                }
            if (d->hello_received != whole)
                return 0;
            memcpy(d->hello_packet + 25, d->hello_body, whole);
            put24(d->hello_packet + 19, 0);
            put24(d->hello_packet + 22, whole);
            size_t payload = 12 + whole;
            d->hello_packet[11] = (uint8_t)(payload >> 8);
            d->hello_packet[12] = (uint8_t)payload;
            data = d->hello_packet;
            size = 13 + payload;
            d->hello_active = false;
        }
        d->hello_delivered = true;
        d->hello_delivered_sequence = sequence;
    }
    if (size > sizeof(d->hello_packet))
        return EWRTC_SECURITY;
    d->incoming = data;
    d->incoming_size = size;
    int ret = advance(d);
    d->incoming = NULL;
    d->incoming_size = 0;
    return ret;
}
static int tick(dtls_adapter *base) {
    if (base->connected)
        return 0;
    mbed_dtls *d = (mbed_dtls *)base;
    if (d->send_error == EWRTC_AGAIN || d->send_error == EWRTC_BACKPRESSURE || timer_get(d) == 2)
        return advance(d);
    return 0;
}
static void destroy(dtls_adapter *base) {
    mbed_dtls *d = (mbed_dtls *)base;
    mbedtls_ssl_free(&d->ssl);
    mbedtls_ssl_cookie_free(&d->cookie);
    mbedtls_ssl_config_free(&d->config);
    mbedtls_x509_crt_free(&d->cert);
    mbedtls_pk_free(&d->key);
    mbedtls_ctr_drbg_free(&d->rng);
    mbedtls_entropy_free(&d->entropy);
    ewrtc_free(&d->base.pal, d);
}
static int set_client(dtls_adapter *base, bool client) {
    mbed_dtls *d = (mbed_dtls *)base;
    mbedtls_ssl_conf_endpoint(&d->config, client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER);
    if (mbedtls_ssl_session_reset(&d->ssl))
        return EWRTC_SECURITY;
    if (!client && mbedtls_ssl_set_client_transport_id(&d->ssl, (const unsigned char *)"ewrtc", 5))
        return EWRTC_SECURITY;
    return 0;
}
static uint64_t deadline(dtls_adapter *base) {
    mbed_dtls *d = (mbed_dtls *)base;
    /* Only final expiry requests retransmission; intermediate does not. */
    return d->timer_final ? d->timer_final : UINT64_MAX;
}
static const dtls_ops ops = {start, receive, tick, destroy, set_client, deadline};

dtls_adapter *dtls_mbedtls_create(const ewrtc_dtls_config *config, int *error) {
    *error = EWRTC_SECURITY;
    const char *remote_fingerprint = config->remote_fingerprint;
    int (*send_packet)(void *, const uint8_t *, size_t) = config->send;
    void *user = config->user;
    if (!send_packet || (remote_fingerprint && strlen(remote_fingerprint) >= 128))
        return NULL;
    mbed_dtls *d = ewrtc_zalloc(&config->pal, sizeof(*d));
    if (!d) {
        *error = EWRTC_NOMEM;
        return NULL;
    }
    d->base.pal = config->pal;
    d->base.ops = &ops;
    d->base.send = send_packet;
    d->base.send_user = user;
    snprintf(d->base.remote_fingerprint, sizeof(d->base.remote_fingerprint), "%s",
             remote_fingerprint ? remote_fingerprint : "");
    mbedtls_entropy_init(&d->entropy);
    mbedtls_ctr_drbg_init(&d->rng);
    mbedtls_pk_init(&d->key);
    mbedtls_x509_crt_init(&d->cert);
    mbedtls_ssl_config_init(&d->config);
    mbedtls_ssl_init(&d->ssl);
    mbedtls_ssl_cookie_init(&d->cookie);
    const unsigned char personalization[] = "ewrtc-dtls";
    if (mbedtls_ctr_drbg_seed(&d->rng, mbedtls_entropy_func, &d->entropy, personalization,
                              sizeof(personalization) - 1) ||
        make_certificate(d) ||
        mbedtls_ssl_config_defaults(&d->config, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
        mbedtls_ssl_conf_dtls_srtp_protection_profiles(&d->config, profiles))
        goto fail;
    mbedtls_ssl_conf_min_tls_version(&d->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&d->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&d->config, mbedtls_ctr_drbg_random, &d->rng);
    if (mbedtls_ssl_cookie_setup(&d->cookie, mbedtls_ctr_drbg_random, &d->rng))
        goto fail;
    mbedtls_ssl_conf_dtls_cookies(&d->config, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check,
                                  &d->cookie);
    mbedtls_ssl_conf_authmode(&d->config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&d->config, verify_cb, NULL);
    if (mbedtls_ssl_conf_own_cert(&d->config, &d->cert, &d->key) ||
        mbedtls_ssl_setup(&d->ssl, &d->config))
        goto fail;
    mbedtls_ssl_set_bio(&d->ssl, d, send_cb, recv_cb, NULL);
    mbedtls_ssl_set_export_keys_cb(&d->ssl, export_keys, d);
    mbedtls_ssl_set_timer_cb(&d->ssl, d, timer_set, timer_get);
    mbedtls_ssl_set_mtu(&d->ssl, EWRTC_MTU);
    if (mbedtls_ssl_set_client_transport_id(&d->ssl, (const unsigned char *)"ewrtc", 5))
        goto fail;
    return &d->base;
fail:
    destroy(&d->base);
    return NULL;
}
