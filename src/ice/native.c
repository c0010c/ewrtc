#include "private.h"
#include "stun/stun.h"
#include "turn/turn.h"
#include <stdio.h>
#include <string.h>
#define NATIVE_REMOTE_MAX 32
typedef struct {
    ewrtc_address addr;
    uint32_t priority;
    char candidate[256];
} remote_candidate;

typedef struct {
    ice_adapter base;
    ewrtc_socket fd;
    ewrtc_crypto_backend crypto;
    char remote_ufrag[256], remote_pwd[256];
    ewrtc_turn *turn;
    ewrtc_address stun_addr, host_addr, relay_addr;
    bool has_stun, has_turn, relay_ready;
    bool relay_only, stun_pending, turn_pending, gathering_done, failed;
    uint64_t gather_started_ms;
    uint8_t stun_transaction[12];
    uint8_t check_transaction[12], consent_transaction[12];
    uint64_t tie_breaker, last_check_ms, last_consent_ms, last_response_ms;
    bool controlling, checking, consent_pending, selected, ever_selected, via_turn;
    ewrtc_address selected_remote;
    char local_selected[256], remote_selected[256];
    remote_candidate remote[NATIVE_REMOTE_MAX];
    size_t remote_count, next_check;
} native_ice;

static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void w64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}
static uint64_t u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}
static int same_addr(const ewrtc_address *a, const ewrtc_address *b) {
    return a->ipv4 == b->ipv4 && a->port == b->port;
}
static int resolve_ipv4(native_ice *n, const char *host, uint16_t port, ewrtc_address *out) {
    if (!ewrtc_address_parse(host, port ? port : 3478, out)) return 0;
    return n->base.pal.network.resolve(n->base.pal.network.ctx, host, port ? port : 3478, out);
}
static int random_text(native_ice *n, char *out, size_t count) {
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t bytes[32];
    if (count >= sizeof(bytes))
        count = sizeof(bytes) - 1;
    int result = ewrtc_random_bytes(&n->base.pal, bytes, count);
    if (result)
        return result;
    for (size_t i = 0; i < count; ++i)
        out[i] = alphabet[bytes[i] % (sizeof(alphabet) - 1)];
    out[count] = 0;
    return 0;
}
static int socket_send(native_ice *n, const ewrtc_address *to, const uint8_t *data, size_t size) {
    return n->base.pal.network.udp_send(n->base.pal.network.ctx, n->fd, to, data, size);
}
static int peer_send(native_ice *n, const ewrtc_address *peer, bool relay, const uint8_t *data,
                     size_t size) {
    return relay ? ewrtc_turn_send(n->turn, peer, data, size) : socket_send(n, peer, data, size);
}
static void emit_candidate(native_ice *n, const ewrtc_address *addr, const char *type,
                           const ewrtc_address *related) {
    char ip[16], relip[16], text[256];
    ewrtc_address_format(addr, ip, sizeof(ip));
    unsigned priority = !strcmp(type, "host")    ? 2130706431u
                        : !strcmp(type, "srflx") ? 1694498815u
                                                 : 16777215u;
    int written = snprintf(text, sizeof(text), "candidate:%s 1 UDP %u %s %u typ %s", type, priority,
                           ip, addr->port, type);
    if (related && written > 0 && (size_t)written < sizeof(text)) {
        ewrtc_address_format(related, relip, sizeof(relip));
        snprintf(text + written, sizeof(text) - (size_t)written, " raddr %s rport %u", relip,
                 related->port);
    }
    n->base.events.candidate(n->base.events.user, text);
}
static int send_stun_binding(native_ice *n) {
    if (!n->has_stun)
        return 0;
    if (ewrtc_stun_random_transaction(&n->base.pal, n->stun_transaction))
        return EWRTC_INVALID;
    ewrtc_stun_writer w;
    ewrtc_stun_begin(&w, STUN_BINDING_REQUEST, n->stun_transaction);
    ewrtc_stun_add_fingerprint(&w);
    return socket_send(n, &n->stun_addr, w.bytes, w.len);
}
static int check_peer(native_ice *n, const ewrtc_address *peer, bool relay, bool consent) {
    if (!n->remote_ufrag[0] || !n->remote_pwd[0])
        return EWRTC_INVALID;
    uint8_t transaction[12];
    if (ewrtc_stun_random_transaction(&n->base.pal, transaction))
        return EWRTC_INVALID;
    memcpy(consent ? n->consent_transaction : n->check_transaction, transaction, 12);
    ewrtc_stun_writer w;
    ewrtc_stun_begin(&w, STUN_BINDING_REQUEST, transaction);
    char username[520];
    snprintf(username, sizeof(username), "%s:%s", n->remote_ufrag, n->base.ufrag);
    uint8_t priority[4], tie[8];
    w32(priority, 1845501695u);
    w64(tie, n->tie_breaker);
    if (ewrtc_stun_add(&w, ATTR_USERNAME, username, strlen(username)) ||
        ewrtc_stun_add(&w, ATTR_PRIORITY, priority, 4) ||
        ewrtc_stun_add(&w, n->controlling ? ATTR_ICE_CONTROLLING : ATTR_ICE_CONTROLLED, tie, 8) ||
        (n->controlling && !consent && ewrtc_stun_add(&w, ATTR_USE_CANDIDATE, NULL, 0)) ||
        ewrtc_stun_add_integrity(&w, (const uint8_t *)n->remote_pwd, strlen(n->remote_pwd),
                                 n->crypto) ||
        ewrtc_stun_add_fingerprint(&w))
        return EWRTC_INVALID;
    int result = peer_send(n, peer, relay, w.bytes, w.len);
    if (consent && result == 0)
        n->consent_pending = true;
    return result;
}
static void handle_peer_packet(native_ice *, const uint8_t *, size_t, const ewrtc_address *, bool);
static void turn_state(void *ctx, const ewrtc_turn_status *status) {
    native_ice *n = ctx;
    if (status->state == EWRTC_TURN_READY) {
        n->relay_addr = status->relayed;
        n->relay_ready = true;
        n->turn_pending = false;
        emit_candidate(n, &n->relay_addr, "relay", &n->host_addr);
    } else if (status->state == EWRTC_TURN_FAILED) {
        n->relay_ready = false;
        n->turn_pending = false;
        if (n->selected && n->via_turn) {
            n->selected = false;
            n->base.events.state(n->base.events.user, EWRTC_DISCONNECTED);
        } else if (n->relay_only && !n->failed) {
            n->failed = true;
            n->base.events.state(n->base.events.user, EWRTC_FAILED);
        }
    }
}
static void turn_recv(void *ctx, const ewrtc_address *peer, const uint8_t *data, size_t size) {
    handle_peer_packet(ctx, data, size, peer, true);
}

static int native_start(ice_adapter *base) {
    native_ice *n = (native_ice *)base;
    n->base.events.state(n->base.events.user, EWRTC_GATHERING);
    n->gather_started_ms = ewrtc_now_ms(&n->base.pal);
    if (!n->relay_only)
        emit_candidate(n, &n->host_addr, "host", NULL);
    n->stun_pending = n->has_stun;
    n->turn_pending = n->has_turn;
    int result = send_stun_binding(n);
    if (result && result != EWRTC_AGAIN)
        return result;
    if (n->turn) {
        result = ewrtc_turn_start(n->turn);
        if (result && n->relay_only)
            return result;
    }
    if (!n->stun_pending && !n->turn_pending) {
        n->base.events.gathering_done(n->base.events.user);
        n->gathering_done = true;
    }
    n->base.events.state(n->base.events.user, EWRTC_CONNECTING);
    return 0;
}
static int native_remote_credentials(ice_adapter *base, const char *ufrag, const char *pwd) {
    native_ice *n = (native_ice *)base;
    if (!ufrag || !pwd || strlen(ufrag) >= sizeof(n->remote_ufrag) ||
        strlen(pwd) >= sizeof(n->remote_pwd))
        return EWRTC_INVALID;
    strcpy(n->remote_ufrag, ufrag);
    strcpy(n->remote_pwd, pwd);
    return 0;
}
static int native_add_candidate(ice_adapter *base, const char *candidate) {
    native_ice *n = (native_ice *)base;
    /* A concealed Chrome host address is learned from its authenticated
     * incoming connectivity check as a peer-reflexive candidate. */
    if (strstr(candidate, ".local "))
        return 0;
    if (n->remote_count == NATIVE_REMOTE_MAX)
        return EWRTC_BACKPRESSURE;
    char foundation[32], transport[8], ip[128], type[16];
    unsigned component, priority, port;
    const char *p = candidate;
    if (!strncmp(p, "a=", 2))
        p += 2;
    if (sscanf(p, "candidate:%31s %u %7s %u %127s %u typ %15s", foundation, &component, transport,
               &priority, ip, &port, type) != 7 ||
        component != 1 || ewrtc_ascii_casecmp(transport, "UDP") || port == 0 || port > 65535)
        return EWRTC_INVALID;
    remote_candidate *r = &n->remote[n->remote_count];
    if (n->base.delivery.notify && ewrtc_address_parse(ip, (uint16_t)port, &r->addr))
        return EWRTC_INVALID; /* Session resolves hostnames off the I/O worker. */
    if (resolve_ipv4(n, ip, (uint16_t)port, &r->addr))
        return EWRTC_INVALID;
    r->priority = priority;
    snprintf(r->candidate, sizeof(r->candidate), "%s", p);
    for (size_t i = 0; i < n->remote_count; ++i)
        if (same_addr(&r->addr, &n->remote[i].addr))
            return 0;
    if (n->turn) {
        int result = ewrtc_turn_add_permission(n->turn, &r->addr);
        if (result && n->relay_only)
            return result;
    }
    n->remote_count++;
    return 0;
}
static int native_end_candidates(ice_adapter *base) {
    (void)base;
    return 0;
}
static int native_send(ice_adapter *base, const uint8_t *data, size_t size) {
    native_ice *n = (native_ice *)base;
    if (!n->selected)
        return EWRTC_STATE;
    if (size > EWRTC_MTU)
        return EWRTC_INVALID;
    return peer_send(n, &n->selected_remote, n->via_turn, data, size);
}
static int native_selected(ice_adapter *base, char *local, size_t lcap, char *remote, size_t rcap) {
    native_ice *n = (native_ice *)base;
    if (!n->selected)
        return EWRTC_STATE;
    snprintf(local, lcap, "%s", n->local_selected);
    snprintf(remote, rcap, "%s", n->remote_selected);
    return 0;
}
static void native_destroy(ice_adapter *base) {
    native_ice *n = (native_ice *)base;
    ewrtc_turn_destroy(n->turn);
    if (n->fd != NULL)
        n->base.pal.network.udp_close(n->base.pal.network.ctx, n->fd);
    ewrtc_free(&n->base.pal, n);
}

static void select_pair(native_ice *n, const ewrtc_address *peer, bool via_turn) {
    n->selected = true;
    n->ever_selected = true;
    n->selected_remote = *peer;
    n->via_turn = via_turn;
    n->last_response_ms = ewrtc_now_ms(&n->base.pal);
    n->last_consent_ms = n->last_response_ms;
    char ip[16];
    const ewrtc_address *local = via_turn ? &n->relay_addr : &n->host_addr;
    ewrtc_address_format(local, ip, sizeof(ip));
    snprintf(n->local_selected, sizeof(n->local_selected), "%s %s:%u", via_turn ? "relay" : "host",
             ip, local->port);
    ewrtc_address_format(peer, ip, sizeof(ip));
    snprintf(n->remote_selected, sizeof(n->remote_selected), "%s:%u", ip, peer->port);
    n->base.events.state(n->base.events.user, EWRTC_CONNECTED);
}
static void send_binding_response(native_ice *n, const ewrtc_stun_packet *packet,
                                  const ewrtc_address *peer, bool via_turn) {
    ewrtc_stun_writer w;
    ewrtc_stun_begin(&w, STUN_BINDING_SUCCESS, packet->transaction);
    if (ewrtc_stun_add_xor_address(&w, ATTR_XOR_MAPPED_ADDRESS, peer) ||
        ewrtc_stun_add_integrity(&w, (const uint8_t *)n->base.pwd, strlen(n->base.pwd),
                                 n->crypto) ||
        ewrtc_stun_add_fingerprint(&w))
        return;
    peer_send(n, peer, via_turn, w.bytes, w.len);
}
static void send_role_conflict(native_ice *n, const ewrtc_stun_packet *packet,
                               const ewrtc_address *peer, bool via_turn) {
    ewrtc_stun_writer w;
    uint8_t code[4] = {0, 0, 4, 87};
    ewrtc_stun_begin(&w, STUN_BINDING_ERROR, packet->transaction);
    if (ewrtc_stun_add(&w, ATTR_ERROR_CODE, code, sizeof(code)) ||
        ewrtc_stun_add_integrity(&w, (const uint8_t *)n->base.pwd, strlen(n->base.pwd),
                                 n->crypto) ||
        ewrtc_stun_add_fingerprint(&w))
        return;
    peer_send(n, peer, via_turn, w.bytes, w.len);
}
static void handle_binding_request(native_ice *n, const ewrtc_stun_packet *packet,
                                   const ewrtc_address *peer, bool via_turn) {
    if (n->relay_only && !via_turn)
        return;
    const ewrtc_stun_attr *username = ewrtc_stun_find(packet, ATTR_USERNAME);
    const ewrtc_stun_attr *controlled = ewrtc_stun_find(packet, ATTR_ICE_CONTROLLED);
    const ewrtc_stun_attr *controlling = ewrtc_stun_find(packet, ATTR_ICE_CONTROLLING);
    if (!username || !ewrtc_stun_find(packet, ATTR_PRIORITY) || (!controlled && !controlling) ||
        !n->remote_pwd[0] ||
        ewrtc_stun_verify_integrity(packet, (const uint8_t *)n->base.pwd, strlen(n->base.pwd),
                                    n->crypto))
        return;
    char expected[520];
    int len = snprintf(expected, sizeof(expected), "%s:%s", n->base.ufrag, n->remote_ufrag);
    if (len < 0 || username->length != (size_t)len ||
        memcmp(username->value, expected, (size_t)len))
        return;
    if (controlled && !n->controlling && controlled->length == 8) {
        if (u64(controlled->value) < n->tie_breaker)
            n->controlling = true;
        else {
            send_role_conflict(n, packet, peer, via_turn);
            return;
        }
    } else if (controlling && n->controlling && controlling->length == 8) {
        if (u64(controlling->value) > n->tie_breaker)
            n->controlling = false;
        else {
            send_role_conflict(n, packet, peer, via_turn);
            return;
        }
    }
    send_binding_response(n, packet, peer, via_turn);
    if (!n->selected && n->controlling) {
        /* An answerer may advertise only mDNS host candidates. Its authenticated
         * check reveals a peer-reflexive address we can nominate as offerer. */
        check_peer(n, peer, via_turn, false);
        n->last_check_ms = ewrtc_now_ms(&n->base.pal);
    } else if (ewrtc_stun_find(packet, ATTR_USE_CANDIDATE) && !n->selected)
        select_pair(n, peer, via_turn);
}
static void handle_binding_success(native_ice *n, const ewrtc_stun_packet *packet,
                                   const ewrtc_address *peer, bool via_turn) {
    if (n->has_stun && same_addr(peer, &n->stun_addr) &&
        !memcmp(packet->transaction, n->stun_transaction, 12)) {
        n->stun_pending = false;
        ewrtc_address mapped;
        if (!ewrtc_stun_decode_xor_address(packet, ewrtc_stun_find(packet, ATTR_XOR_MAPPED_ADDRESS),
                                           &mapped))
            emit_candidate(n, &mapped, "srflx", &n->host_addr);
        return;
    }
    if (ewrtc_stun_verify_integrity(packet, (const uint8_t *)n->remote_pwd, strlen(n->remote_pwd),
                                    n->crypto))
        return;
    if (n->selected && !memcmp(packet->transaction, n->consent_transaction, 12) &&
        same_addr(peer, &n->selected_remote) && via_turn == n->via_turn) {
        n->last_response_ms = ewrtc_now_ms(&n->base.pal);
        n->consent_pending = false;
    } else if (!n->selected && !memcmp(packet->transaction, n->check_transaction, 12) &&
               n->controlling) {
        select_pair(n, peer, via_turn);
    }
}
static void handle_binding_error(native_ice *n, const ewrtc_stun_packet *packet) {
    const ewrtc_stun_attr *error = ewrtc_stun_find(packet, ATTR_ERROR_CODE);
    if (!error || error->length < 4 || ((error->value[2] & 7) * 100 + error->value[3]) != 487 ||
        memcmp(packet->transaction, n->check_transaction, 12) ||
        ewrtc_stun_verify_integrity(packet, (const uint8_t *)n->remote_pwd, strlen(n->remote_pwd),
                                    n->crypto))
        return;
    n->controlling = !n->controlling;
    n->last_check_ms = 0;
}
static void handle_peer_packet(native_ice *n, const uint8_t *data, size_t size,
                               const ewrtc_address *peer, bool via_turn) {
    if (size >= 20 && data[0] < 4) {
        ewrtc_stun_packet packet;
        if (ewrtc_stun_parse(data, size, &packet))
            return;
        if (packet.type == STUN_BINDING_REQUEST)
            handle_binding_request(n, &packet, peer, via_turn);
        else if (packet.type == STUN_BINDING_SUCCESS)
            handle_binding_success(n, &packet, peer, via_turn);
        else if (packet.type == STUN_BINDING_ERROR)
            handle_binding_error(n, &packet);
        return;
    }
    if (n->selected && same_addr(peer, &n->selected_remote) && via_turn == n->via_turn)
        n->base.events.recv(n->base.events.user, data, size);
}

static int native_drain(ice_adapter *base, unsigned *packets, unsigned *events, bool *more) {
    (void)events;
    *more = false;
    native_ice *n = (native_ice *)base;
    uint8_t data[2048];
    while (*packets) {
        if (base->delivery.stopped && base->delivery.stopped(base->delivery.user)) return 0;
        if (base->delivery.yield && base->delivery.yield(base->delivery.user)) {
            *more = true; return 0;
        }
        ewrtc_address peer;
        size_t got = 0;
        int result = n->base.pal.network.udp_receive(n->base.pal.network.ctx, n->fd, &peer, data,
                                                     sizeof(data), &got);
        if (result == EWRTC_AGAIN)
            return 0;
        --*packets;
        if (result == EWRTC_INVALID)
            continue; /* discard oversized datagrams */
        if (result)
            return result;
        if (n->turn) {
            int turn_result = ewrtc_turn_receive(n->turn, &peer, data, got);
            if (turn_result != EWRTC_AGAIN && turn_result != EWRTC_STATE)
                continue;
        }
        handle_peer_packet(n, data, got, &peer, false);
    }
    *more = *packets == 0;
    return 0;
}
static int native_timers(ice_adapter *base) {
    native_ice *n = (native_ice *)base;
    if (n->turn)
        (void)ewrtc_turn_tick(n->turn); /* state callback handles relay failure */
    uint64_t now = ewrtc_now_ms(&n->base.pal);
    if (n->stun_pending && now - n->gather_started_ms >= 5000)
        n->stun_pending = false;
    /* TURN owns its transaction deadlines; do not signal end-of-candidates
     * while a retried allocation can still produce a relay candidate. */
    if (!n->gathering_done && !n->stun_pending && !n->turn_pending) {
        n->gathering_done = true;
        n->base.events.gathering_done(n->base.events.user);
    }
    if (!n->selected && n->remote_pwd[0] && n->remote_count && now - n->last_check_ms >= 300) {
        remote_candidate *r = &n->remote[n->next_check++ % n->remote_count];
        if (!n->relay_only)
            check_peer(n, &r->addr, false, false);
        if (n->relay_ready && ewrtc_turn_has_permission(n->turn, &r->addr))
            check_peer(n, &r->addr, true, false);
        n->last_check_ms = now;
    }
    if (n->selected && now - n->last_consent_ms >= 15000) {
        check_peer(n, &n->selected_remote, n->via_turn, true);
        n->last_consent_ms = now;
    }
    if (n->selected && now - n->last_response_ms > 30000) {
        n->selected = false;
        n->base.events.state(n->base.events.user, EWRTC_DISCONNECTED);
    }
    if (!n->ever_selected && !n->failed && n->base.started && now - n->gather_started_ms >= 30000) {
        n->failed = true;
        n->base.events.state(n->base.events.user, EWRTC_FAILED);
    }
    return 0;
}

static ewrtc_socket native_socket(ice_adapter *base) { return ((native_ice *)base)->fd; }
static uint64_t native_deadline(ice_adapter *base) {
    native_ice *n = (native_ice *)base;
    if (n->failed) return UINT64_MAX;
    uint64_t next = ewrtc_turn_next_deadline(n->turn), at;
    if (n->stun_pending && n->gather_started_ms + 5000 < next) next = n->gather_started_ms + 5000;
    if (!n->gathering_done && !n->stun_pending && !n->turn_pending) next = 0;
    if (!n->selected && n->remote_pwd[0] && n->remote_count) {
        at = n->last_check_ms + 300;
        if (at < next) next = at;
    }
    if (n->selected) {
        at = n->last_consent_ms + 15000;
        if (at < next) next = at;
        at = n->last_response_ms + 30001;
        if (at < next) next = at;
    }
    if (!n->ever_selected && n->gather_started_ms + 30000 < next) next = n->gather_started_ms + 30000;
    return next;
}
static int native_tick(ice_adapter *base) {
    unsigned packets = 64, events = 32; bool more;
    int result = native_drain(base, &packets, &events, &more);
    return result ? result : native_timers(base);
}

static const ice_ops ops = {native_start,         native_tick,           native_remote_credentials,
                            native_add_candidate, native_end_candidates, native_send,
                            native_selected,      native_destroy, native_socket, native_drain,
                            native_timers, native_deadline};

ice_adapter *ice_native_create(const ewrtc_ice_config *config, const ewrtc_ice_events *events,
                               int *error) {
    *error = EWRTC_NOMEM;
    native_ice *n = ewrtc_zalloc(&config->pal, sizeof(*n));
    if (!n)
        return NULL;
    *error = EWRTC_INVALID;
    n->base.pal = config->pal;
    n->base.delivery = config->delivery;
    n->base.ops = &ops;
    n->base.events = *events;
    n->crypto = config->crypto_backend;
    n->relay_only = config->relay_only != 0;
    if (n->relay_only && (!config->turn_host || !*config->turn_host))
        goto fail;
    n->controlling = config->controlling;
    *error = EWRTC_SECURITY;
    if (random_text(n, n->base.ufrag, 8) || random_text(n, n->base.pwd, 24) ||
        ewrtc_random_bytes(&n->base.pal, &n->tie_breaker, sizeof(n->tie_breaker)))
        goto fail;
    ewrtc_address bind_addr = {0};
    if ((*error = n->base.pal.network.udp_open(n->base.pal.network.ctx, &bind_addr, &n->fd)) ||
        (*error = n->base.pal.network.udp_local(n->base.pal.network.ctx, n->fd, &bind_addr)))
        goto fail;
    ewrtc_address addresses[16];
    size_t count = 0;
    if ((*error = n->base.pal.network.interfaces(n->base.pal.network.ctx, addresses, 16, &count)))
        goto fail;
    if (!count) {
        *error = EWRTC_IO;
        goto fail;
    }
    n->host_addr = addresses[0];
    n->host_addr.port = bind_addr.port;
    if (config->stun_host) {
        if ((*error = resolve_ipv4(n, config->stun_host, config->stun_port, &n->stun_addr)))
            goto fail;
        n->has_stun = true;
    }
    if (config->turn_host) {
        ewrtc_turn_config tc = {.pal = config->pal, .crypto_backend = n->crypto,
            .socket = n->fd, .server_host = config->turn_host, .server_port = config->turn_port,
            .username = config->turn_username, .password = config->turn_password,
            .state = turn_state, .recv = turn_recv, .user = n};
        *error = ewrtc_turn_create(&tc, &n->turn);
        if (*error)
            goto fail;
        n->has_turn = true;
    }
    *error = EWRTC_OK;
    return &n->base;
fail:
    native_destroy(&n->base);
    return NULL;
}
