#ifndef EWRTC_CRYPTO_PRIVATE_H
#define EWRTC_CRYPTO_PRIVATE_H
#include "crypto/crypto.h"
typedef struct {
    int (*hmac_sha1)(const void *, size_t, const void *, size_t, uint8_t[20]);
    int (*md5)(const void *, size_t, uint8_t[16]);
} crypto_ops;
extern const crypto_ops ewrtc_crypto_openssl_ops;
extern const crypto_ops ewrtc_crypto_mbedtls_ops;
#endif
