#ifndef EWRTC_STUN_H
#define EWRTC_STUN_H
#include "pal/pal.h"
#include "crypto/crypto.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define STUN_COOKIE 0x2112a442u
#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_SUCCESS 0x0101
#define STUN_BINDING_ERROR 0x0111
#define STUN_ALLOCATE_REQUEST 0x0003
#define STUN_ALLOCATE_SUCCESS 0x0103
#define STUN_ALLOCATE_ERROR 0x0113
#define STUN_REFRESH_REQUEST 0x0004
#define STUN_REFRESH_SUCCESS 0x0104
#define STUN_REFRESH_ERROR 0x0114
#define STUN_SEND_INDICATION 0x0016
#define STUN_DATA_INDICATION 0x0017
#define STUN_PERMISSION_REQUEST 0x0008
#define STUN_PERMISSION_SUCCESS 0x0108
#define STUN_PERMISSION_ERROR 0x0118

#define ATTR_USERNAME 0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_ERROR_CODE 0x0009
#define ATTR_LIFETIME 0x000d
#define ATTR_REALM 0x0014
#define ATTR_NONCE 0x0015
#define ATTR_XOR_PEER_ADDRESS 0x0012
#define ATTR_DATA 0x0013
#define ATTR_XOR_RELAYED_ADDRESS 0x0016
#define ATTR_REQUESTED_TRANSPORT 0x0019
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_PRIORITY 0x0024
#define ATTR_USE_CANDIDATE 0x0025
#define ATTR_FINGERPRINT 0x8028
#define ATTR_ICE_CONTROLLED 0x8029
#define ATTR_ICE_CONTROLLING 0x802a

typedef struct {
    uint8_t bytes[2048];
    size_t len;
} ewrtc_stun_writer;
typedef struct {
    uint16_t type, length;
    const uint8_t *value;
    size_t offset;
} ewrtc_stun_attr;
typedef struct {
    const uint8_t *bytes;
    size_t len;
    uint16_t type;
    uint8_t transaction[12];
    ewrtc_stun_attr attrs[48];
    size_t count;
} ewrtc_stun_packet;

int ewrtc_stun_begin(ewrtc_stun_writer *, uint16_t, const uint8_t transaction[12]);
int ewrtc_stun_add(ewrtc_stun_writer *, uint16_t, const void *, size_t);
int ewrtc_stun_add_xor_address(ewrtc_stun_writer *, uint16_t, const ewrtc_address *);
int ewrtc_stun_add_integrity(ewrtc_stun_writer *, const uint8_t *, size_t, ewrtc_crypto_backend);
int ewrtc_stun_add_fingerprint(ewrtc_stun_writer *);
int ewrtc_stun_parse(const uint8_t *, size_t, ewrtc_stun_packet *);
const ewrtc_stun_attr *ewrtc_stun_find(const ewrtc_stun_packet *, uint16_t);
int ewrtc_stun_verify_integrity(const ewrtc_stun_packet *, const uint8_t *, size_t,
                                ewrtc_crypto_backend);
int ewrtc_stun_decode_xor_address(const ewrtc_stun_packet *, const ewrtc_stun_attr *,
                                  ewrtc_address *);
int ewrtc_stun_long_term_key(const ewrtc_pal *, const char *, const char *, const char *,
                             ewrtc_crypto_backend, uint8_t out[16]);
int ewrtc_stun_random_transaction(const ewrtc_pal *, uint8_t out[12]);
#ifdef __cplusplus
}
#endif
#endif
