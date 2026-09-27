#include "private.h"
#include <limits.h>
static const crypto_ops *backend_ops(ewrtc_crypto_backend b) {
#if EWRTC_WITH_OPENSSL
    if (b == EWRTC_CRYPTO_OPENSSL)
        return &ewrtc_crypto_openssl_ops;
#endif
#if EWRTC_WITH_MBEDTLS
    if (b == EWRTC_CRYPTO_MBEDTLS)
        return &ewrtc_crypto_mbedtls_ops;
#endif
    (void)b;
    return NULL;
}
int ewrtc_crypto_available(ewrtc_crypto_backend b) { return backend_ops(b) != NULL; }
int ewrtc_crypto_hmac_sha1(ewrtc_crypto_backend b, const void *data, size_t size,
                           const void *key, size_t key_size, uint8_t out[20]) {
    if (!data || !key || !out || key_size > INT_MAX)
        return EWRTC_INVALID;
    const crypto_ops *ops = backend_ops(b);
    return ops ? ops->hmac_sha1(data, size, key, key_size, out) : EWRTC_UNSUPPORTED;
}
int ewrtc_crypto_md5(ewrtc_crypto_backend b, const void *data, size_t size, uint8_t out[16]) {
    if (!data || !out)
        return EWRTC_INVALID;
    const crypto_ops *ops = backend_ops(b);
    return ops ? ops->md5(data, size, out) : EWRTC_UNSUPPORTED;
}
