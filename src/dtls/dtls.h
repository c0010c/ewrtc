#ifndef EWRTC_DTLS_H
#define EWRTC_DTLS_H
#include "pal/pal.h"
#include "ewrtc/backends.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ewrtc_dtls ewrtc_dtls;
typedef struct {
    ewrtc_pal pal;
    ewrtc_dtls_backend backend;
    const char *remote_fingerprint;
    int (*send)(void *, const uint8_t *, size_t);
    void *user;
} ewrtc_dtls_config;
/* DTLS 1.2, SRTP_AES128_CM_SHA1_80. Serialize all calls. NULL remote
 * fingerprint creates only the local identity; set_peer is required before start.
 * Existing callers supplying a fingerprint default to server role. */
int ewrtc_dtls_create(const ewrtc_dtls_config *, ewrtc_dtls **);
int ewrtc_dtls_set_peer(ewrtc_dtls *, const char *fingerprint, bool client);
int ewrtc_dtls_start(ewrtc_dtls *);
int ewrtc_dtls_receive(ewrtc_dtls *, const uint8_t *, size_t);
/* Absolute monotonic milliseconds; UINT64_MAX means no timer. */
uint64_t ewrtc_dtls_next_deadline(ewrtc_dtls *);
int ewrtc_dtls_tick(ewrtc_dtls *);
const char *ewrtc_dtls_fingerprint(const ewrtc_dtls *);
int ewrtc_dtls_connected(const ewrtc_dtls *);
/* RFC 5764 exporter order: client key, server key, client salt, server salt. */
int ewrtc_dtls_export_keys(const ewrtc_dtls *, uint8_t out[60]);
void ewrtc_dtls_destroy(ewrtc_dtls *);

#ifdef __cplusplus
}
#endif
#endif
