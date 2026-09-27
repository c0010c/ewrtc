#include "private.h"
#include <mbedtls/md.h>
static int hmac_sha1(const void *data, size_t size, const void *key, size_t key_size, uint8_t out[20]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    return info && !mbedtls_md_hmac(info, key, key_size, data, size, out) ? EWRTC_OK : EWRTC_SECURITY;
}
static int md5(const void *data, size_t size, uint8_t out[16]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    return info && !mbedtls_md(info, data, size, out) ? EWRTC_OK : EWRTC_SECURITY;
}
const crypto_ops ewrtc_crypto_mbedtls_ops = {hmac_sha1, md5};
