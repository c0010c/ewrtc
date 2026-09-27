#ifndef EWRTC_CRYPTO_H
#define EWRTC_CRYPTO_H
#include "common/common.h"
#include "ewrtc/backends.h"
#ifdef __cplusplus
extern "C" {
#endif
int ewrtc_crypto_available(ewrtc_crypto_backend);
int ewrtc_crypto_hmac_sha1(ewrtc_crypto_backend, const void *, size_t, const void *key,
                           size_t key_size, uint8_t out[20]);
int ewrtc_crypto_md5(ewrtc_crypto_backend, const void *, size_t, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
#endif
