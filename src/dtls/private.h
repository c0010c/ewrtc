#ifndef EWRTC_DTLS_PRIVATE_H
#define EWRTC_DTLS_PRIVATE_H
#include "dtls/dtls.h"

enum {
    DTLS_SHA256_SIZE = 32,
    DTLS_FINGERPRINT_SIZE = DTLS_SHA256_SIZE * 3
};

typedef ewrtc_dtls dtls_adapter;
typedef struct {
    int (*start)(dtls_adapter *);
    int (*receive)(dtls_adapter *, const uint8_t *, size_t);
    int (*tick)(dtls_adapter *);
    void (*destroy)(dtls_adapter *);
    int (*set_client)(dtls_adapter *, bool);
    uint64_t (*deadline)(dtls_adapter *);
} dtls_ops;

struct ewrtc_dtls {
    ewrtc_pal pal;
    bool started;
    const dtls_ops *ops;
    char fingerprint[128];
    char remote_fingerprint[128];
    uint8_t key_material[60];
    bool connected;
    uint64_t retry_at;
    int (*send)(void *, const uint8_t *, size_t);
    void *send_user;
};

dtls_adapter *dtls_openssl_create(const ewrtc_dtls_config *, int *error);
dtls_adapter *dtls_mbedtls_create(const ewrtc_dtls_config *, int *error);

/* Called only after ewrtc_dtls_create validates the config; base is zeroed. */
void dtls_adapter_init(dtls_adapter *, const ewrtc_dtls_config *, const dtls_ops *);
int dtls_format_fingerprint(const uint8_t digest[DTLS_SHA256_SIZE], char *, size_t);

#endif
