#ifndef EWRTC_BACKENDS_H
#define EWRTC_BACKENDS_H

/* Backend selection values shared by configuration and statistics.
 * Protocol objects and operations are internal to the SDK. */
typedef enum { EWRTC_CRYPTO_OPENSSL = 0, EWRTC_CRYPTO_MBEDTLS = 1 } ewrtc_crypto_backend;
typedef enum { EWRTC_ICE_NATIVE = 0, EWRTC_ICE_LIBJUICE = 1 } ewrtc_ice_backend;
typedef enum { EWRTC_DTLS_OPENSSL = 0, EWRTC_DTLS_MBEDTLS = 1 } ewrtc_dtls_backend;

#endif
