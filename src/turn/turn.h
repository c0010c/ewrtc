#ifndef EWRTC_TURN_H
#define EWRTC_TURN_H
#include "pal/pal.h"
#include "crypto/crypto.h"
#ifdef __cplusplus
extern "C" {
#endif
#define EWRTC_TURN_MAX_PERMISSIONS 32u
typedef struct ewrtc_turn ewrtc_turn;
typedef enum { EWRTC_TURN_NEW, EWRTC_TURN_ALLOCATING, EWRTC_TURN_READY, EWRTC_TURN_FAILED } ewrtc_turn_state;
typedef struct {
    ewrtc_turn_state state;
    ewrtc_result error;
    ewrtc_address relayed;
    uint64_t expires_ms;
    size_t permissions;
} ewrtc_turn_status;
typedef struct {
    ewrtc_pal pal;
    ewrtc_crypto_backend crypto_backend;
    ewrtc_socket socket; /* Borrowed; never read or closed by TURN. */
    const char *server_host, *username, *password;
    uint16_t server_port; /* 0: 3478 */
    void (*state)(void *, const ewrtc_turn_status *);
    void (*recv)(void *, const ewrtc_address *, const uint8_t *, size_t);
    void *user;
} ewrtc_turn_config;
/* IPv4/UDP, long-term credentials, Send/Data indications. All calls serialized;
 * callbacks synchronously borrow their arguments. Do not destroy in a callback.
 * Config/services are copied. PAL contexts and socket must outlive the object. */
int ewrtc_turn_create(const ewrtc_turn_config *, ewrtc_turn **);
int ewrtc_turn_start(ewrtc_turn *);
/* Caller feeds datagrams from its socket. Unrelated/duplicate packets: AGAIN.
 * INVALID/SECURITY means discard this packet; terminal failures are in status. */
int ewrtc_turn_receive(ewrtc_turn *, const ewrtc_address *, const uint8_t *, size_t);
/* Absolute monotonic milliseconds; UINT64_MAX means no timer. */
uint64_t ewrtc_turn_next_deadline(ewrtc_turn *);
int ewrtc_turn_tick(ewrtc_turn *);
/* Permissions are per peer IP, not port. Requests may precede allocation.
 * Pending control sends/retransmits are retained internally, including AGAIN. */
int ewrtc_turn_add_permission(ewrtc_turn *, const ewrtc_address *);
int ewrtc_turn_has_permission(const ewrtc_turn *, const ewrtc_address *);
/* Pending permission/send would block: AGAIN. Payload is never queued/copied. */
int ewrtc_turn_send(ewrtc_turn *, const ewrtc_address *, const uint8_t *, size_t);
int ewrtc_turn_get_status(const ewrtc_turn *, ewrtc_turn_status *);
/* Best-effort zero-lifetime Refresh, no wait, no socket close; NULL is allowed. */
void ewrtc_turn_destroy(ewrtc_turn *);
#ifdef __cplusplus
}
#endif
#endif
