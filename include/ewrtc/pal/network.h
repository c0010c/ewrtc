#ifndef EWRTC_PAL_NETWORK_H
#define EWRTC_PAL_NETWORK_H
#include "../common.h"
typedef void *ewrtc_socket;
/* All UDP operations are nonblocking. AGAIN means no datagram/temporarily
 * unavailable. Successful send consumes the complete datagram synchronously.
 * Receive discards truncated datagrams and returns INVALID, never partial data.
 * Open clears its output on failure; caller closes every successful socket once.
 * DNS is synchronous. Addresses use host byte order, no OS address types. */
typedef struct {
    void *ctx;
    int (*udp_open)(void *, const ewrtc_address *, ewrtc_socket *);
    void (*udp_close)(void *, ewrtc_socket);
    int (*udp_local)(void *, ewrtc_socket, ewrtc_address *);
    int (*udp_send)(void *, ewrtc_socket, const ewrtc_address *, const void *, size_t);
    int (*udp_receive)(void *, ewrtc_socket, ewrtc_address *, void *, size_t, size_t *);
    int (*resolve)(void *, const char *, uint16_t, ewrtc_address *);
    /* Returns active non-loopback IPv4 addresses, or loopback when none exist. */
    int (*interfaces)(void *, ewrtc_address *, size_t capacity, size_t *count);
} ewrtc_network;
#endif
