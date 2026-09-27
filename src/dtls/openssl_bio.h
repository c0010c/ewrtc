#ifndef EWRTC_DTLS_OPENSSL_BIO_H
#define EWRTC_DTLS_OPENSSL_BIO_H
#include "pal/pal.h"
#include <openssl/bio.h>

/* In-memory datagrams for OpenSSL 1.1.1. The caller owns the method and must
 * keep it alive until every BIO created with it has been freed. */
BIO_METHOD *ewrtc_openssl_bio_method(void);
BIO *ewrtc_openssl_bio_new(BIO_METHOD *, const ewrtc_pal *);
#endif
