#include "private.h"
#include "openssl_bio.h"
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <sys/time.h>

typedef struct {
    dtls_adapter base;
    SSL_CTX *ctx;
    SSL *ssl;
    BIO_METHOD *bio_method;
    uint8_t pending[2048];
    size_t pending_size;
} openssl_dtls;

static int verify_any(int ok, X509_STORE_CTX *ctx) {
    (void)ok;
    (void)ctx;
    /* SDP fingerprints are verified after the handshake. */
    return 1;
}

static int format_fingerprint(X509 *cert, char *out, size_t size) {
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!X509_digest(cert, EVP_sha256(), digest, &length) || length != DTLS_SHA256_SIZE)
        return EWRTC_SECURITY;
    return dtls_format_fingerprint(digest, out, size);
}

static int make_certificate(const ewrtc_pal *pal, SSL_CTX *ctx, char *fingerprint, size_t size) {
    EVP_PKEY_CTX *key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    int ret = EWRTC_SECURITY;
    if (!key_ctx || EVP_PKEY_keygen_init(key_ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(key_ctx, &key) <= 0)
        goto done;
    cert = X509_new();
    if (!cert || X509_set_version(cert, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) != 1 ||
        !ASN1_TIME_set(X509_getm_notBefore(cert), (time_t)(ewrtc_utc_us(pal) / 1000000) - 60) ||
        !ASN1_TIME_set(X509_getm_notAfter(cert), (time_t)(ewrtc_utc_us(pal) / 1000000) + 86400) ||
        X509_set_pubkey(cert, key) != 1)
        goto done;
    X509_NAME *name = X509_get_subject_name(cert);
    if (!name ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"ewrtc", -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(cert, name) != 1 || X509_sign(cert, key, EVP_sha256()) <= 0 ||
        SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1 ||
        format_fingerprint(cert, fingerprint, size))
        goto done;
    ret = 0;
done:
    X509_free(cert);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(key_ctx);
    return ret;
}

static int flush_out(openssl_dtls *d) {
    BIO *out = SSL_get_wbio(d->ssl);
    while (d->pending_size || BIO_ctrl_pending(out)) {
        if (!d->pending_size) {
            int n = BIO_read(out, d->pending, sizeof(d->pending));
            if (n <= 0)
                return EWRTC_SECURITY;
            d->pending_size = (size_t)n;
        }
        int result = d->base.send(d->base.send_user, d->pending, d->pending_size);
        if (result)
            return result;
        d->pending_size = 0;
    }
    return 0;
}

static int finish(openssl_dtls *d) {
    if (!SSL_is_init_finished(d->ssl))
        return 0;
    X509 *peer = SSL_get_peer_certificate(d->ssl);
    char actual[128];
    if (!peer) {
        EWRTC_LOG(&d->base.pal, EWRTC_LOG_ERROR, "DTLS", "openssl peer certificate missing");
        return EWRTC_SECURITY;
    }
    int valid = format_fingerprint(peer, actual, sizeof(actual)) == 0 &&
                ewrtc_ascii_casecmp(actual, d->base.remote_fingerprint) == 0;
    X509_free(peer);
    if (!valid) {
        EWRTC_LOG(&d->base.pal, EWRTC_LOG_ERROR, "DTLS", "openssl peer fingerprint mismatch");
        return EWRTC_SECURITY;
    }
    const SRTP_PROTECTION_PROFILE *profile = SSL_get_selected_srtp_profile(d->ssl);
    if (!profile || strcmp(profile->name, "SRTP_AES128_CM_SHA1_80")) {
        EWRTC_LOG(&d->base.pal, EWRTC_LOG_ERROR, "DTLS", "openssl SRTP profile negotiation failed");
        return EWRTC_SECURITY;
    }
    if (SSL_export_keying_material(d->ssl, d->base.key_material, sizeof(d->base.key_material),
                                   "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) != 1) {
        EWRTC_LOG(&d->base.pal, EWRTC_LOG_ERROR, "DTLS", "openssl SRTP key export failed");
        return EWRTC_SECURITY;
    }
    d->base.connected = true;
    return 0;
}

static int advance(openssl_dtls *d) {
    int sent = flush_out(d);
    if (sent)
        return sent;
    ERR_clear_error(); /* Multiple sessions share this thread error queue. */
    int result = SSL_do_handshake(d->ssl);
    int err = result == 1 ? SSL_ERROR_NONE : SSL_get_error(d->ssl, result);
    sent = flush_out(d);
    if (sent)
        return sent;
    if (result == 1)
        return finish(d);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;
    if (ewrtc_log_enabled(&d->base.pal, EWRTC_LOG_ERROR)) {
        char detail[256];
        ERR_error_string_n(ERR_peek_last_error(), detail, sizeof(detail));
        EWRTC_LOG(&d->base.pal, EWRTC_LOG_ERROR, "DTLS", "openssl handshake failed ssl_error=%d detail=%s", err, detail);
    }
    return EWRTC_SECURITY;
}
static int start(dtls_adapter *base) {
    return advance((openssl_dtls *)base);
}
static int receive(dtls_adapter *base, const uint8_t *data, size_t size) {
    openssl_dtls *d = (openssl_dtls *)base;
    if (size > INT32_MAX || BIO_write(SSL_get_rbio(d->ssl), data, (int)size) != (int)size)
        return EWRTC_SECURITY;
    return advance(d);
}
static int tick(dtls_adapter *base) {
    openssl_dtls *d = (openssl_dtls *)base;
    if (base->connected)
        return 0;
    int sent = flush_out(d);
    if (sent)
        return sent;
    if (SSL_is_init_finished(d->ssl))
        return finish(d);
    struct timeval wait;
    if (DTLSv1_get_timeout(d->ssl, &wait) == 1 && wait.tv_sec == 0 && wait.tv_usec == 0) {
        if (DTLSv1_handle_timeout(d->ssl) < 0)
            return EWRTC_SECURITY;
        return flush_out(d);
    }
    return 0;
}
static void destroy(dtls_adapter *base) {
    openssl_dtls *d = (openssl_dtls *)base;
    SSL_free(d->ssl);
    SSL_CTX_free(d->ctx);
    BIO_meth_free(d->bio_method);
    ewrtc_free(&d->base.pal, d);
}
static int set_client(dtls_adapter *base, bool client) {
    openssl_dtls *d = (openssl_dtls *)base;
    if (client)
        SSL_set_connect_state(d->ssl);
    else
        SSL_set_accept_state(d->ssl);
    return 0;
}
static uint64_t deadline(dtls_adapter *base) {
    openssl_dtls *d = (openssl_dtls *)base;
    struct timeval wait;
    if (DTLSv1_get_timeout(d->ssl, &wait) != 1)
        return UINT64_MAX;
    return ewrtc_now_ms(&base->pal) + (uint64_t)wait.tv_sec * 1000 +
           ((uint64_t)wait.tv_usec + 999) / 1000;
}
static const dtls_ops ops = {start, receive, tick, destroy, set_client, deadline};

dtls_adapter *dtls_openssl_create(const ewrtc_dtls_config *config, int *error) {
    *error = EWRTC_SECURITY;
    openssl_dtls *d = ewrtc_zalloc(&config->pal, sizeof(*d));
    if (!d) {
        *error = EWRTC_NOMEM;
        return NULL;
    }
    dtls_adapter_init(&d->base, config, &ops);
    d->ctx = SSL_CTX_new(DTLS_method());
    if (!d->ctx || SSL_CTX_set_min_proto_version(d->ctx, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(d->ctx, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_tlsext_use_srtp(d->ctx, "SRTP_AES128_CM_SHA1_80") != 0 ||
        make_certificate(&d->base.pal, d->ctx, d->base.fingerprint, sizeof(d->base.fingerprint)))
        goto fail;
    SSL_CTX_set_verify(d->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_any);
    d->ssl = SSL_new(d->ctx);
    if (!d->ssl)
        goto fail;
    d->bio_method = ewrtc_openssl_bio_method();
    if (!d->bio_method)
        goto fail;
    BIO *incoming = ewrtc_openssl_bio_new(d->bio_method, &d->base.pal);
    BIO *outgoing = ewrtc_openssl_bio_new(d->bio_method, &d->base.pal);
    if (!incoming || !outgoing) {
        BIO_free(incoming);
        BIO_free(outgoing);
        goto fail;
    }
    SSL_set_bio(d->ssl, incoming, outgoing);
    SSL_set_accept_state(d->ssl);
    SSL_set_options(d->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(d->ssl, EWRTC_MTU);
    return &d->base;
fail:
    destroy(&d->base);
    return NULL;
}
