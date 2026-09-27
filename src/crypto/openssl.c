#include "private.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
static int hmac_sha1(const void *data, size_t size, const void *key, size_t key_size, uint8_t out[20]) {
    unsigned len = 0;
    return HMAC(EVP_sha1(), key, (int)key_size, data, size, out, &len) && len == 20 ? EWRTC_OK : EWRTC_SECURITY;
}
static int md5(const void *data, size_t size, uint8_t out[16]) {
    unsigned len = 0;
    return EVP_Digest(data, size, out, &len, EVP_md5(), NULL) == 1 && len == 16 ? EWRTC_OK : EWRTC_SECURITY;
}
const crypto_ops ewrtc_crypto_openssl_ops = {hmac_sha1, md5};
