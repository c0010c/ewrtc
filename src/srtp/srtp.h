#ifndef EWRTC_SRTP_H
#define EWRTC_SRTP_H
#include "pal/pal.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ewrtc_srtp ewrtc_srtp;
/* AES128_CM_SHA1_80 only. Keys are exporter order; server=false reverses directions. */
int ewrtc_srtp_create(const ewrtc_pal *, const uint8_t keys[60], bool server, ewrtc_srtp **);
void ewrtc_srtp_destroy(ewrtc_srtp *);
/* In place; protection needs at least 32 spare bytes. *size updated on success. */
int ewrtc_srtp_protect(ewrtc_srtp *, bool rtcp, uint8_t *, size_t capacity, size_t *size);
int ewrtc_srtp_unprotect(ewrtc_srtp *, bool rtcp, uint8_t *, size_t *size);

#ifdef __cplusplus
}
#endif
#endif
