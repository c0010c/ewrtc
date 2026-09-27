#ifndef EWRTC_COMMON_H
#define EWRTC_COMMON_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define EWRTC_MAX_SDP 65536u
#define EWRTC_MAX_FRAME (2u * 1024u * 1024u)
#define EWRTC_MAX_CANDIDATE 512u
#define EWRTC_MAX_OPUS_PACKET_BYTES 1172u
typedef enum {
    EWRTC_OK = 0,
    EWRTC_INVALID = -1,
    EWRTC_STATE = -2,
    EWRTC_NOMEM = -3,
    EWRTC_BACKPRESSURE = -4,
    EWRTC_UNSUPPORTED = -5,
    EWRTC_IO = -6,
    EWRTC_SECURITY = -7,
    EWRTC_AGAIN = -8,
    EWRTC_TIMEOUT = -9
} ewrtc_result;

/* Zero-initialized direction means sendrecv. */
typedef enum {
    EWRTC_SENDRECV = 0, EWRTC_SENDONLY, EWRTC_RECVONLY, EWRTC_INACTIVE
} ewrtc_direction;

/* IPv4 numeric value and port are in host byte order; no OS layout. */
typedef struct {
    uint32_t ipv4;
    uint16_t port;
} ewrtc_address;
int ewrtc_address_parse(const char *, uint16_t, ewrtc_address *);
int ewrtc_address_format(const ewrtc_address *, char *, size_t);

typedef enum {
    EWRTC_NEW = 0,
    EWRTC_GATHERING,
    EWRTC_CONNECTING,
    EWRTC_CONNECTED,
    EWRTC_DISCONNECTED,
    EWRTC_FAILED,
    EWRTC_CLOSED
} ewrtc_state;

#ifdef __cplusplus
}
#endif
#endif
