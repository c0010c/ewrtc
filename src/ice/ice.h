#ifndef EWRTC_ICE_H
#define EWRTC_ICE_H
#include "pal/pal.h"
#include "crypto/crypto.h"
#include "ewrtc/backends.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ewrtc_ice ewrtc_ice;
/* Optional queue integration. Hooks may run on backend producer threads.
 * allocate/release must be paired; BACKPRESSURE on media means drop, not failure.
 * notify also fires on the first delivery error, even without a queued node.
 * Hooks/user must survive destroy, which joins all producers. */
typedef struct {
    void *user;
    void *(*allocate)(void *, size_t, bool control, int *error);
    void (*release)(void *, void *);
    void (*notify)(void *);
    bool (*stopped)(void *);
    bool (*yield)(void *); /* owner-thread drain must return to process deferred control */
} ewrtc_ice_delivery;
typedef struct {
    ewrtc_pal pal;
    ewrtc_ice_backend backend;
    ewrtc_crypto_backend crypto_backend;
    const char *stun_host, *turn_host, *turn_username, *turn_password;
    uint16_t stun_port, turn_port;
    int relay_only;
    ewrtc_ice_delivery delivery;
    bool controlling; /* initial native ICE role; libjuice resolves roles internally */
} ewrtc_ice_config;
typedef struct {
    void (*state)(void *, ewrtc_state);
    void (*candidate)(void *, const char *);
    void (*gathering_done)(void *);
    void (*recv)(void *, const uint8_t *, size_t);
    void *user;
} ewrtc_ice_events;
/* All four event callbacks are required.
 * Protocol event callbacks are emitted by serialized start/tick/drain/timers/
 * input calls, including libjuice. Delivery hooks may run on producer threads.
 * Serialize all calls. Do not destroy the object inside its callback. */
int ewrtc_ice_create(const ewrtc_ice_config *, const ewrtc_ice_events *, ewrtc_ice **);
int ewrtc_ice_start(ewrtc_ice *);
/* Absolute monotonic milliseconds; UINT64_MAX means no timer. */
uint64_t ewrtc_ice_next_deadline(ewrtc_ice *);
int ewrtc_ice_tick(ewrtc_ice *);
/* Event-loop calls: drain decrements shared turn budgets and reports backlog.
 * Native socket is borrowed; unregister it before destroy. */
ewrtc_socket ewrtc_ice_socket(ewrtc_ice *);
int ewrtc_ice_drain(ewrtc_ice *, unsigned *packets, unsigned *events, bool *more);
int ewrtc_ice_timers(ewrtc_ice *);
int ewrtc_ice_remote_credentials(ewrtc_ice *, const char *, const char *);
int ewrtc_ice_add_candidate(ewrtc_ice *, const char *);
int ewrtc_ice_end_candidates(ewrtc_ice *);
int ewrtc_ice_send(ewrtc_ice *, const uint8_t *, size_t);
int ewrtc_ice_selected(ewrtc_ice *, char *, size_t, char *, size_t);
int ewrtc_ice_credentials(ewrtc_ice *, char *, size_t, char *, size_t);
void ewrtc_ice_destroy(ewrtc_ice *);

#ifdef __cplusplus
}
#endif
#endif
