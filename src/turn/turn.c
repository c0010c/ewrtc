#include "turn/turn.h"
#include "stun/stun.h"
#include <string.h>

#define TRANSACTION_TIMEOUT_MS 8000u
#define PERMISSION_LIFETIME_MS 300000u
#define REFRESH_INTERVAL_MS 60000u

typedef struct {
    uint8_t id[12], key[16];
    uint8_t *wire;
    size_t size;
    uint16_t method;
    unsigned challenges, attempts;
    uint64_t next_ms, deadline_ms;
    bool authenticated;
} transaction;
typedef struct {
    ewrtc_address peer;
    uint64_t expires_ms, refresh_ms;
    transaction tx;
} permission;
struct ewrtc_turn {
    ewrtc_turn_config cfg;
    ewrtc_turn_status status;
    ewrtc_address server;
    char username[128], password[128], realm[128], nonce[256];
    uint8_t key[16];
    bool authenticated;
    uint64_t refresh_ms;
    transaction allocation;
    permission peers[EWRTC_TURN_MAX_PERMISSIONS];
    size_t count;
};
static bool same_address(const ewrtc_address *a, const ewrtc_address *b) {
    return a->ipv4 == b->ipv4 && a->port == b->port;
}
static void clear_transaction(ewrtc_turn *t, transaction *tx) {
    ewrtc_free(&t->cfg.pal, tx->wire);
    memset(tx, 0, sizeof(*tx));
}
static void notify(ewrtc_turn *t) {
    if (t->cfg.state)
        t->cfg.state(t->cfg.user, &t->status);
}
static int fail(ewrtc_turn *t, int error) {
    if (t->status.state != EWRTC_TURN_FAILED) {
        EWRTC_LOG(&t->cfg.pal, EWRTC_LOG_ERROR, "TURN", "failed state=%d code=%d", t->status.state, error);
        t->status.state = EWRTC_TURN_FAILED;
        t->status.error = (ewrtc_result)error;
        clear_transaction(t, &t->allocation);
        for (size_t i = 0; i < t->count; ++i)
            clear_transaction(t, &t->peers[i].tx);
        notify(t);
    }
    return error;
}
static int send_wire(ewrtc_turn *t, const uint8_t *data, size_t size) {
    return t->cfg.pal.network.udp_send(t->cfg.pal.network.ctx, t->cfg.socket,
                                     &t->server, data, size);
}
static int add_auth(ewrtc_turn *t, ewrtc_stun_writer *w) {
    int r;
    if (!t->authenticated)
        return EWRTC_OK;
    if ((r = ewrtc_stun_add(w, ATTR_USERNAME, t->username, strlen(t->username))) ||
        (r = ewrtc_stun_add(w, ATTR_REALM, t->realm, strlen(t->realm))) ||
        (r = ewrtc_stun_add(w, ATTR_NONCE, t->nonce, strlen(t->nonce))))
        return r;
    return ewrtc_stun_add_integrity(w, t->key, sizeof(t->key), t->cfg.crypto_backend);
}
static int transmit(ewrtc_turn *t, transaction *tx, uint64_t now) {
    int r = send_wire(t, tx->wire, tx->size);
    if (r == EWRTC_AGAIN) {
        tx->next_ms = now + 20;
        return EWRTC_OK;
    }
    if (r)
        return fail(t, r);
    unsigned shift = tx->attempts < 3 ? tx->attempts : 3;
    tx->next_ms = now + (500u << shift);
    ++tx->attempts;
    return EWRTC_OK;
}
static int request(ewrtc_turn *t, transaction *tx, uint16_t method,
                   const ewrtc_address *peer, unsigned challenges) {
    ewrtc_stun_writer w;
    uint8_t id[12];
    int r = ewrtc_stun_random_transaction(&t->cfg.pal, id);
    if (r)
        return fail(t, r);
    ewrtc_stun_begin(&w, method, id);
    if (method == STUN_ALLOCATE_REQUEST) {
        const uint8_t udp[4] = {17, 0, 0, 0};
        if ((r = ewrtc_stun_add(&w, ATTR_REQUESTED_TRANSPORT, udp, sizeof(udp))))
            return fail(t, r);
    }
    if (peer && (r = ewrtc_stun_add_xor_address(&w, ATTR_XOR_PEER_ADDRESS, peer)))
        return fail(t, r);
    if ((r = add_auth(t, &w)) || (r = ewrtc_stun_add_fingerprint(&w)))
        return fail(t, r);
    uint8_t *copy = ewrtc_alloc(&t->cfg.pal, w.len);
    if (!copy)
        return fail(t, EWRTC_NOMEM);
    memcpy(copy, w.bytes, w.len);
    clear_transaction(t, tx);
    tx->wire = copy;
    tx->size = w.len;
    memcpy(tx->id, id, sizeof(id));
    memcpy(tx->key, t->key, sizeof(tx->key));
    tx->authenticated = t->authenticated;
    tx->method = method;
    tx->challenges = challenges;
    uint64_t now = ewrtc_now_ms(&t->cfg.pal);
    tx->deadline_ms = now + TRANSACTION_TIMEOUT_MS;
    return transmit(t, tx, now);
}
static int copy_attribute(char *dst, size_t capacity, const ewrtc_stun_attr *a) {
    if (!a || !a->length || a->length >= capacity || memchr(a->value, 0, a->length))
        return EWRTC_INVALID;
    memcpy(dst, a->value, a->length);
    dst[a->length] = 0;
    return EWRTC_OK;
}
int ewrtc_turn_create(const ewrtc_turn_config *c, ewrtc_turn **out) {
    if (!out)
        return EWRTC_INVALID;
    *out = NULL;
    if (!c || !c->socket || !c->server_host || !*c->server_host || !c->username ||
        !c->password || strlen(c->username) >= 128 || strlen(c->password) >= 128 ||
        ewrtc_pal_validate(&c->pal, EWRTC_PAL_ALLOCATOR | EWRTC_PAL_MONOTONIC |
                           EWRTC_PAL_RANDOM | EWRTC_PAL_UDP_SEND | EWRTC_PAL_DNS))
        return EWRTC_INVALID;
    if (!ewrtc_crypto_available(c->crypto_backend))
        return EWRTC_UNSUPPORTED;
    ewrtc_turn *t = ewrtc_zalloc(&c->pal, sizeof(*t));
    if (!t)
        return EWRTC_NOMEM;
    t->cfg = *c;
    strcpy(t->username, c->username);
    strcpy(t->password, c->password);
    t->cfg.username = t->username;
    t->cfg.password = t->password;
    t->cfg.server_host = NULL; /* consumed synchronously */
    int r = c->pal.network.resolve(c->pal.network.ctx, c->server_host,
                                   c->server_port ? c->server_port : 3478, &t->server);
    if (r) {
        ewrtc_free(&c->pal, t);
        return r;
    }
    *out = t;
    return EWRTC_OK;
}
int ewrtc_turn_start(ewrtc_turn *t) {
    if (!t)
        return EWRTC_INVALID;
    if (t->status.state != EWRTC_TURN_NEW)
        return EWRTC_STATE;
    t->status.state = EWRTC_TURN_ALLOCATING;
    notify(t);
    return request(t, &t->allocation, STUN_ALLOCATE_REQUEST, NULL, 0);
}
int ewrtc_turn_add_permission(ewrtc_turn *t, const ewrtc_address *peer) {
    if (!t || !peer || !peer->ipv4 || !peer->port)
        return EWRTC_INVALID;
    if (t->status.state == EWRTC_TURN_FAILED)
        return EWRTC_STATE;
    for (size_t i = 0; i < t->count; ++i)
        if (t->peers[i].peer.ipv4 == peer->ipv4)
            return EWRTC_OK;
    if (t->count == EWRTC_TURN_MAX_PERMISSIONS)
        return EWRTC_BACKPRESSURE;
    permission *p = &t->peers[t->count++];
    p->peer = *peer;
    if (t->status.state == EWRTC_TURN_READY)
        return request(t, &p->tx, STUN_PERMISSION_REQUEST, &p->peer, 0);
    return EWRTC_OK;
}
int ewrtc_turn_has_permission(const ewrtc_turn *t, const ewrtc_address *peer) {
    if (!t || !peer || t->status.state != EWRTC_TURN_READY)
        return 0;
    uint64_t now = ewrtc_now_ms(&t->cfg.pal);
    if (now >= t->status.expires_ms)
        return 0;
    for (size_t i = 0; i < t->count; ++i)
        if (t->peers[i].peer.ipv4 == peer->ipv4 && now < t->peers[i].expires_ms)
            return 1;
    return 0;
}
int ewrtc_turn_receive(ewrtc_turn *t, const ewrtc_address *from, const uint8_t *data, size_t size) {
    if (!t || !from || !data)
        return EWRTC_INVALID;
    if (t->status.state == EWRTC_TURN_NEW || t->status.state == EWRTC_TURN_FAILED)
        return EWRTC_STATE;
    if (!same_address(from, &t->server))
        return EWRTC_AGAIN;
    ewrtc_stun_packet packet;
    if (ewrtc_stun_parse(data, size, &packet))
        return EWRTC_INVALID;
    if (packet.type == STUN_DATA_INDICATION) {
        ewrtc_address peer;
        const ewrtc_stun_attr *payload = ewrtc_stun_find(&packet, ATTR_DATA);
        if (!payload || payload->length > EWRTC_MTU || ewrtc_stun_decode_xor_address(
                &packet, ewrtc_stun_find(&packet, ATTR_XOR_PEER_ADDRESS), &peer))
            return EWRTC_INVALID;
        if (!ewrtc_turn_has_permission(t, &peer))
            return EWRTC_AGAIN;
        if (t->cfg.recv)
            t->cfg.recv(t->cfg.user, &peer, payload->value, payload->length);
        return EWRTC_OK;
    }
    transaction *tx = NULL;
    permission *p = NULL;
    if (t->allocation.wire && !memcmp(t->allocation.id, packet.transaction, 12))
        tx = &t->allocation;
    for (size_t i = 0; !tx && i < t->count; ++i)
        if (t->peers[i].tx.wire && !memcmp(t->peers[i].tx.id, packet.transaction, 12)) {
            p = &t->peers[i];
            tx = &p->tx;
        }
    if (!tx || (packet.type != (tx->method | 0x0100) && packet.type != (tx->method | 0x0110)))
        return EWRTC_AGAIN;
    if (ewrtc_now_ms(&t->cfg.pal) >= tx->deadline_ms)
        return fail(t, EWRTC_TIMEOUT);
    bool error = packet.type == (tx->method | 0x0110);
    const ewrtc_stun_attr *err = ewrtc_stun_find(&packet, ATTR_ERROR_CODE);
    unsigned code = err && err->length >= 4 ? (err->value[2] & 7u) * 100 + err->value[3] : 0;
    /* RFC 8489 9.2.4: 438 may omit integrity. When present, verify it;
     * ordinary authenticated responses always require integrity. */
    bool challenge = error && (code == 401 || code == 438);
    if (tx->authenticated && (!challenge || ewrtc_stun_find(&packet, ATTR_MESSAGE_INTEGRITY)) &&
        ewrtc_stun_verify_integrity(&packet, tx->key, sizeof(tx->key), t->cfg.crypto_backend))
        return EWRTC_SECURITY;
    if (error && (code == 401 || code == 438)) {
        EWRTC_LOG(&t->cfg.pal, EWRTC_LOG_DEBUG, "TURN", "challenge method=0x%04x status=%u attempt=%u",
                  (unsigned)tx->method, code, tx->challenges + 1);
        if ((code == 401 && tx->authenticated) || tx->challenges >= 2)
            return fail(t, EWRTC_SECURITY);
        char realm[128], nonce[256];
        if (copy_attribute(realm, sizeof(realm), ewrtc_stun_find(&packet, ATTR_REALM)) ||
            copy_attribute(nonce, sizeof(nonce), ewrtc_stun_find(&packet, ATTR_NONCE)))
            return EWRTC_INVALID;
        uint8_t key[16];
        int r = ewrtc_stun_long_term_key(&t->cfg.pal, t->username, realm, t->password,
                                        t->cfg.crypto_backend, key);
        if (r)
            return fail(t, r);
        strcpy(t->realm, realm);
        strcpy(t->nonce, nonce);
        memcpy(t->key, key, sizeof(key));
        t->authenticated = true;
        return request(t, tx, tx->method, p ? &p->peer : NULL, tx->challenges + 1);
    }
    if (error)
        return code ? fail(t, EWRTC_IO) : EWRTC_INVALID;
    uint64_t now = ewrtc_now_ms(&t->cfg.pal);
    if (p) {
        p->expires_ms = now + PERMISSION_LIFETIME_MS;
        p->refresh_ms = now + REFRESH_INTERVAL_MS;
        clear_transaction(t, tx);
        return EWRTC_OK;
    }
    const ewrtc_stun_attr *lifetime = ewrtc_stun_find(&packet, ATTR_LIFETIME);
    if (!lifetime || lifetime->length != 4)
        return EWRTC_INVALID;
    uint64_t duration = (uint64_t)ewrtc_read_u32(lifetime->value) * 1000;
    if (!duration)
        return fail(t, EWRTC_TIMEOUT);
    bool allocating = tx->method == STUN_ALLOCATE_REQUEST;
    ewrtc_address relay = t->status.relayed;
    if (allocating && ewrtc_stun_decode_xor_address(&packet,
            ewrtc_stun_find(&packet, ATTR_XOR_RELAYED_ADDRESS), &relay))
        return EWRTC_INVALID;
    t->status.relayed = relay;
    t->status.expires_ms = now + duration;
    t->refresh_ms = now + (duration / 2 < REFRESH_INTERVAL_MS ? duration / 2 : REFRESH_INTERVAL_MS);
    clear_transaction(t, tx);
    t->status.state = EWRTC_TURN_READY;
    if (allocating) {
        notify(t);
        for (size_t i = 0; i < t->count; ++i) {
            if (t->peers[i].tx.wire)
                continue; /* callback may already have requested it */
            int r = request(t, &t->peers[i].tx, STUN_PERMISSION_REQUEST, &t->peers[i].peer, 0);
            if (r)
                return r;
        }
    }
    return EWRTC_OK;
}
int ewrtc_turn_tick(ewrtc_turn *t) {
    if (!t)
        return EWRTC_INVALID;
    if (t->status.state == EWRTC_TURN_NEW)
        return EWRTC_STATE;
    if (t->status.state == EWRTC_TURN_FAILED)
        return t->status.error;
    uint64_t now = ewrtc_now_ms(&t->cfg.pal);
    if (t->status.state == EWRTC_TURN_READY && now >= t->status.expires_ms)
        return fail(t, EWRTC_TIMEOUT);
    for (size_t i = 0; i <= t->count; ++i) {
        transaction *tx = i ? &t->peers[i - 1].tx : &t->allocation;
        if (!tx->wire)
            continue;
        if (now >= tx->deadline_ms)
            return fail(t, EWRTC_TIMEOUT);
        if (now >= tx->next_ms) {
            int r = transmit(t, tx, now);
            if (r)
                return r;
        }
    }
    if (t->status.state == EWRTC_TURN_READY) {
        if (!t->allocation.wire && now >= t->refresh_ms) {
            int r = request(t, &t->allocation, STUN_REFRESH_REQUEST, NULL, 0);
            if (r)
                return r;
        }
        for (size_t i = 0; i < t->count; ++i) {
            permission *p = &t->peers[i];
            if (!p->tx.wire && now >= p->refresh_ms) {
                int r = request(t, &p->tx, STUN_PERMISSION_REQUEST, &p->peer, 0);
                if (r)
                    return r;
            }
        }
    }
    return EWRTC_OK;
}
int ewrtc_turn_send(ewrtc_turn *t, const ewrtc_address *peer, const uint8_t *data, size_t size) {
    if (!t || !peer || !data || !size || size > EWRTC_MTU)
        return EWRTC_INVALID;
    if (t->status.state == EWRTC_TURN_FAILED)
        return EWRTC_STATE;
    if (!ewrtc_turn_has_permission(t, peer))
        return EWRTC_AGAIN;
    uint8_t id[12];
    int r = ewrtc_stun_random_transaction(&t->cfg.pal, id);
    if (r)
        return r;
    ewrtc_stun_writer w;
    ewrtc_stun_begin(&w, STUN_SEND_INDICATION, id);
    if ((r = ewrtc_stun_add_xor_address(&w, ATTR_XOR_PEER_ADDRESS, peer)) ||
        (r = ewrtc_stun_add(&w, ATTR_DATA, data, size)) || (r = ewrtc_stun_add_fingerprint(&w)))
        return r;
    return send_wire(t, w.bytes, w.len);
}
int ewrtc_turn_get_status(const ewrtc_turn *t, ewrtc_turn_status *out) {
    if (!t || !out)
        return EWRTC_INVALID;
    *out = t->status;
    out->permissions = 0;
    for (size_t i = 0; i < t->count; ++i)
        out->permissions += ewrtc_turn_has_permission(t, &t->peers[i].peer) != 0;
    return EWRTC_OK;
}
void ewrtc_turn_destroy(ewrtc_turn *t) {
    if (!t)
        return;
    if (t->status.relayed.port) {
        uint8_t id[12], lifetime[4] = {0};
        ewrtc_stun_writer w;
        if (!ewrtc_stun_random_transaction(&t->cfg.pal, id) &&
            !ewrtc_stun_begin(&w, STUN_REFRESH_REQUEST, id) &&
            !ewrtc_stun_add(&w, ATTR_LIFETIME, lifetime, sizeof(lifetime)) &&
            !add_auth(t, &w) && !ewrtc_stun_add_fingerprint(&w))
            (void)send_wire(t, w.bytes, w.len);
    }
    clear_transaction(t, &t->allocation);
    for (size_t i = 0; i < t->count; ++i)
        clear_transaction(t, &t->peers[i].tx);
    ewrtc_free(&t->cfg.pal, t);
}

uint64_t ewrtc_turn_next_deadline(ewrtc_turn *t) {
    if (!t || t->status.state == EWRTC_TURN_NEW || t->status.state == EWRTC_TURN_FAILED)
        return UINT64_MAX;
    uint64_t next = UINT64_MAX;
    if (t->status.state == EWRTC_TURN_READY) {
        next = t->status.expires_ms;
        if (!t->allocation.wire && t->refresh_ms < next) next = t->refresh_ms;
        for (size_t i = 0; i < t->count; ++i)
            if (!t->peers[i].tx.wire && t->peers[i].refresh_ms < next)
                next = t->peers[i].refresh_ms;
    }
    for (size_t i = 0; i <= t->count; ++i) {
        transaction *tx = i ? &t->peers[i-1].tx : &t->allocation;
        if (!tx->wire) continue;
        if (tx->next_ms < next) next = tx->next_ms;
        if (tx->deadline_ms < next) next = tx->deadline_ms;
    }
    return next;
}
