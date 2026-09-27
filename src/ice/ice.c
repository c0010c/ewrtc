#include "private.h"
#include <stdio.h>
#include <string.h>
int ewrtc_ice_create(const ewrtc_ice_config *c, const ewrtc_ice_events *e, ewrtc_ice **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!c || (!!c->delivery.allocate != !!c->delivery.release) || !e || !e->state || !e->candidate || !e->gathering_done || !e->recv ||
        (c->backend != EWRTC_ICE_NATIVE && c->backend != EWRTC_ICE_LIBJUICE) ||
        (c->relay_only && (!c->turn_host || !*c->turn_host)))
        return EWRTC_INVALID;
    unsigned caps = EWRTC_PAL_ALLOCATOR;
    if (c->backend == EWRTC_ICE_NATIVE)
        caps |= EWRTC_PAL_MONOTONIC | EWRTC_PAL_RANDOM | EWRTC_PAL_NETWORK;
    else
        caps |= EWRTC_PAL_MUTEX;
    if (ewrtc_pal_validate(&c->pal, caps))
        return EWRTC_INVALID;
    if (c->backend == EWRTC_ICE_NATIVE) {
#if EWRTC_WITH_NATIVE_ICE
        if (!ewrtc_crypto_available(c->crypto_backend))
            return EWRTC_UNSUPPORTED;
        int error;
        *out = ice_native_create(c, e, &error);
        if (!*out)
            return error;
#else
        return EWRTC_UNSUPPORTED;
#endif
    } else {
#if EWRTC_WITH_LIBJUICE
        int error;
        *out = ice_juice_create(c, e, &error);
        if (!*out) return error;
#else
        return EWRTC_UNSUPPORTED;
#endif
    }
    return *out ? 0 : EWRTC_IO;
}
int ewrtc_ice_start(ewrtc_ice *s) {
    if (!s)
        return EWRTC_INVALID;
    if (s->started)
        return EWRTC_STATE;
    s->started = true;
    return s->ops->start(s);
}
int ewrtc_ice_tick(ewrtc_ice *s) {
    return !s ? EWRTC_INVALID : !s->started ? EWRTC_STATE : s->ops->tick(s);
}
int ewrtc_ice_remote_credentials(ewrtc_ice *s, const char *u, const char *p) {
    return !s || !u || !p ? EWRTC_INVALID : s->ops->remote_credentials(s, u, p);
}
int ewrtc_ice_add_candidate(ewrtc_ice *s, const char *c) {
    return !s || !c || strlen(c) > EWRTC_MAX_CANDIDATE ? EWRTC_INVALID
                                                       : s->ops->add_candidate(s, c);
}
int ewrtc_ice_end_candidates(ewrtc_ice *s) {
    return !s ? EWRTC_INVALID : s->ops->end_candidates(s);
}
int ewrtc_ice_send(ewrtc_ice *s, const uint8_t *d, size_t n) {
    return !s || !d || !n || n > EWRTC_MTU ? EWRTC_INVALID : s->ops->send(s, d, n);
}
int ewrtc_ice_selected(ewrtc_ice *s, char *l, size_t ln, char *r, size_t rn) {
    return !s || !l || !r || !ln || !rn ? EWRTC_INVALID : s->ops->selected(s, l, ln, r, rn);
}
int ewrtc_ice_credentials(ewrtc_ice *s, char *u, size_t un, char *p, size_t pn) {
    if (!s || !u || !p || un <= strlen(s->ufrag) || pn <= strlen(s->pwd))
        return EWRTC_INVALID;
    strcpy(u, s->ufrag);
    strcpy(p, s->pwd);
    return 0;
}
void ewrtc_ice_destroy(ewrtc_ice *s) {
    if (s)
        s->ops->destroy(s);
}

ewrtc_socket ewrtc_ice_socket(ewrtc_ice *s) {
    return s ? s->ops->get_socket(s) : NULL;
}
int ewrtc_ice_drain(ewrtc_ice *s, unsigned *packets, unsigned *events, bool *more) {
    if (!s || !packets || !events || !more) return EWRTC_INVALID;
    *more = false;
    return s->started ? s->ops->drain(s, packets, events, more) : EWRTC_STATE;
}
int ewrtc_ice_timers(ewrtc_ice *s) {
    return !s ? EWRTC_INVALID : !s->started ? EWRTC_STATE : s->ops->timers(s);
}
uint64_t ewrtc_ice_next_deadline(ewrtc_ice *s) {
    return s && s->started ? s->ops->deadline(s) : UINT64_MAX;
}
