#include "turn/turn.h"
#include "stun/stun.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint64_t now;
    size_t live, allocations, fail_at;
    unsigned random_id, count, received, states;
    int send_error, random_error, dns_error;
    ewrtc_stun_writer sent[128];
    ewrtc_address server, peer;
    ewrtc_turn_status status;
} fake;
static void *allocate(void *ctx, size_t n) {
    fake *f = ctx;
    if (++f->allocations == f->fail_at)
        return NULL;
    void *p = malloc(n);
    if (p) ++f->live;
    return p;
}
static void release(void *ctx, void *p) {
    fake *f = ctx;
    if (p) { assert(f->live); --f->live; free(p); }
}
static uint64_t now(void *ctx) { return ((fake *)ctx)->now; }
static int random_bytes(void *ctx, void *p, size_t n) {
    fake *f = ctx;
    if (f->random_error) return f->random_error;
    memset(p, ++f->random_id, n);
    return EWRTC_OK;
}
static int resolve(void *ctx, const char *host, uint16_t port, ewrtc_address *out) {
    fake *f = ctx;
    assert(!strcmp(host, "turn.test") && port == 3478);
    *out = f->server;
    return f->dns_error;
}
static int send_udp(void *ctx, ewrtc_socket socket, const ewrtc_address *to, const void *p, size_t n) {
    fake *f = ctx;
    assert(socket == f && to->ipv4 == f->server.ipv4 && to->port == f->server.port);
    if (f->send_error) return f->send_error;
    assert(f->count < 128 && n <= sizeof(f->sent[0].bytes));
    ewrtc_stun_writer *w = &f->sent[f->count++];
    memcpy(w->bytes, p, n); w->len = n;
    return EWRTC_OK;
}
static void state(void *ctx, const ewrtc_turn_status *status) {
    fake *f = ctx; f->status = *status; ++f->states;
}
static void recv_data(void *ctx, const ewrtc_address *peer, const uint8_t *p, size_t n) {
    fake *f = ctx;
    assert(n == 3 && !memcmp(p, "abc", 3));
    f->peer = *peer; ++f->received;
}
static ewrtc_crypto_backend crypto(void) {
    return ewrtc_crypto_available(EWRTC_CRYPTO_OPENSSL) ? EWRTC_CRYPTO_OPENSSL : EWRTC_CRYPTO_MBEDTLS;
}
static ewrtc_turn_config config(fake *f) {
    f->server = (ewrtc_address){0x7f000001, 3478};
    return (ewrtc_turn_config){
        .pal = {.memory = {f, allocate, NULL, release}, .clock = {f, now, NULL},
            .random = {f, random_bytes}, .network = {.ctx = f, .udp_send = send_udp, .resolve = resolve}},
        .crypto_backend = crypto(), .socket = f, .server_host = "turn.test",
        .username = "test", .password = "pass", .state = state, .recv = recv_data, .user = f};
}
static void sign_response(ewrtc_stun_writer *w) {
    uint8_t key[16];
    assert(!ewrtc_crypto_md5(crypto(), "test:realm:pass", 15, key));
    assert(!ewrtc_stun_add_integrity(w, key, sizeof(key), crypto()));
    assert(!ewrtc_stun_add_fingerprint(w));
}
static void response(fake *f, unsigned request, uint16_t type, ewrtc_stun_writer *w) {
    assert(request < f->count);
    assert(!ewrtc_stun_begin(w, type, f->sent[request].bytes + 8));
}
static int feed(ewrtc_turn *t, fake *f, ewrtc_stun_writer *w) {
    return ewrtc_turn_receive(t, &f->server, w->bytes, w->len);
}
static void challenge(ewrtc_turn *t, fake *f, unsigned request, unsigned code, uint16_t type) {
    bool signed_error = code == 438;
    code %= 1000;
    ewrtc_stun_writer w;
    response(f, request, type, &w);
    const uint8_t error[4] = {0, 0, (uint8_t)(code / 100), (uint8_t)(code % 100)};
    assert(!ewrtc_stun_add(&w, ATTR_ERROR_CODE, error, 4));
    assert(!ewrtc_stun_add(&w, ATTR_REALM, "realm", 5));
    assert(!ewrtc_stun_add(&w, ATTR_NONCE, code == 401 ? "nonce1" : "nonce2", 6));
    if (signed_error) sign_response(&w);
    assert(!feed(t, f, &w));
}
static void allocation_success(ewrtc_turn *t, fake *f, unsigned request, bool refresh) {
    ewrtc_stun_writer w;
    response(f, request, refresh ? STUN_REFRESH_SUCCESS : STUN_ALLOCATE_SUCCESS, &w);
    const ewrtc_address relay = {0xc0000201, 50000};
    uint8_t lifetime[4]; ewrtc_write_u32(lifetime, 120);
    assert(!ewrtc_stun_add(&w, ATTR_LIFETIME, lifetime, 4));
    if (!refresh) assert(!ewrtc_stun_add_xor_address(&w, ATTR_XOR_RELAYED_ADDRESS, &relay));
    sign_response(&w);
    assert(!feed(t, f, &w));
}
static ewrtc_turn *ready(fake *f) {
    ewrtc_turn_config c = config(f); ewrtc_turn *t;
    assert(!ewrtc_turn_create(&c, &t));
    memset(&c.pal, 0, sizeof(c.pal)); /* copied services */
    assert(ewrtc_turn_next_deadline(t) == UINT64_MAX);
    assert(!ewrtc_turn_start(t));
    assert(ewrtc_turn_next_deadline(t) == f->now + 500);
    challenge(t, f, 0, 401, STUN_ALLOCATE_ERROR);
    allocation_success(t, f, 1, false);
    assert(f->status.state == EWRTC_TURN_READY);
    assert(ewrtc_turn_next_deadline(t) == f->now + 60000);
    return t;
}
static void test_permissions(void) {
    fake f = {0}; ewrtc_turn *t = ready(&f);
    ewrtc_address a = {0xc0000202, 1000}, b = {0xc0000203, 2000}, a2 = {a.ipv4, 3000};
    assert(!ewrtc_turn_add_permission(t, &a));
    assert(!ewrtc_turn_add_permission(t, &b));
    assert(!ewrtc_turn_add_permission(t, &a2) && f.count == 4);
    assert(ewrtc_turn_send(t, &a, (const uint8_t *)"abc", 3) == EWRTC_AGAIN);
    ewrtc_stun_writer w;
    response(&f, 2, STUN_REFRESH_SUCCESS, &w); sign_response(&w);
    assert(feed(t, &f, &w) == EWRTC_AGAIN); /* method mismatch */
    response(&f, 2, STUN_PERMISSION_SUCCESS, &w);
    assert(feed(t, &f, &w) == EWRTC_SECURITY); /* unsigned */
    sign_response(&w); assert(!feed(t, &f, &w));
    assert(ewrtc_turn_has_permission(t, &a) && ewrtc_turn_has_permission(t, &a2));
    assert(!ewrtc_turn_has_permission(t, &b));
    assert(feed(t, &f, &w) == EWRTC_AGAIN); /* duplicate */
    response(&f, 3, STUN_PERMISSION_SUCCESS, &w); sign_response(&w); assert(!feed(t, &f, &w));
    ewrtc_turn_status status;
    assert(!ewrtc_turn_get_status(t, &status) && status.permissions == 2);
    assert(!ewrtc_turn_send(t, &a, (const uint8_t *)"abc", 3));
    assert(ewrtc_read_u16(f.sent[4].bytes) == STUN_SEND_INDICATION);
    f.send_error = EWRTC_AGAIN;
    assert(ewrtc_turn_send(t, &a, (const uint8_t *)"abc", 3) == EWRTC_AGAIN);
    f.send_error = 0;
    uint8_t id[12] = {0};
    ewrtc_stun_begin(&w, STUN_DATA_INDICATION, id);
    ewrtc_stun_add_xor_address(&w, ATTR_XOR_PEER_ADDRESS, &b);
    ewrtc_stun_add(&w, ATTR_DATA, "abc", 3);
    assert(ewrtc_turn_receive(t, &a, w.bytes, w.len) == EWRTC_AGAIN);
    assert(!feed(t, &f, &w) && f.received == 1 && f.peer.ipv4 == b.ipv4);
    f.now = 60000;
    unsigned refresh = f.count;
    assert(!ewrtc_turn_tick(t));
    assert(ewrtc_read_u16(f.sent[refresh].bytes) == STUN_REFRESH_REQUEST);
    challenge(t, &f, refresh, 1438, STUN_REFRESH_ERROR);
    allocation_success(t, &f, f.count - 1, true);
    for (unsigned i = refresh + 1; i <= refresh + 2; ++i) {
        response(&f, i, STUN_PERMISSION_SUCCESS, &w); sign_response(&w); assert(!feed(t, &f, &w));
    }
    assert(!ewrtc_turn_get_status(t, &status) && status.expires_ms == 180000);
    unsigned sent = f.count;
    ewrtc_turn_destroy(t);
    assert(!f.live && f.count == sent + 1); /* no close service even exists */
    ewrtc_stun_packet packet;
    assert(!ewrtc_stun_parse(f.sent[sent].bytes, f.sent[sent].len, &packet));
    assert(packet.type == STUN_REFRESH_REQUEST);
    assert(ewrtc_read_u32(ewrtc_stun_find(&packet, ATTR_LIFETIME)->value) == 0);
}
static void test_retry_and_failure(void) {
    fake f = {.send_error = EWRTC_AGAIN};
    ewrtc_turn_config c = config(&f); ewrtc_turn *t;
    assert(!ewrtc_turn_create(&c, &t));
    assert(ewrtc_turn_tick(t) == EWRTC_STATE);
    assert(!ewrtc_turn_start(t) && !f.count);
    assert(ewrtc_turn_start(t) == EWRTC_STATE);
    f.send_error = 0; f.now = 20;
    assert(!ewrtc_turn_tick(t) && f.count == 1);
    f.now = 520;
    assert(!ewrtc_turn_tick(t) && f.count == 2);
    assert(f.sent[0].len == f.sent[1].len && !memcmp(f.sent[0].bytes, f.sent[1].bytes, f.sent[0].len));
    f.now = 8000;
    assert(ewrtc_turn_tick(t) == EWRTC_TIMEOUT && f.status.state == EWRTC_TURN_FAILED);
    unsigned states = f.states;
    assert(ewrtc_turn_tick(t) == EWRTC_TIMEOUT && f.states == states);
    ewrtc_turn_destroy(t); assert(!f.live);
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        memset(&f, 0, sizeof(f)); c = config(&f);
        if (scenario == 0) f.fail_at = 1;
        if (scenario == 1) f.dns_error = EWRTC_IO;
        if (scenario == 2) c.pal.network.udp_send = NULL;
        int r = ewrtc_turn_create(&c, &t);
        if (scenario < 3) { assert(r && !t && !f.live); continue; }
        assert(!r);
        if (scenario == 3) f.fail_at = f.allocations + 1;
        if (scenario == 4) f.random_error = EWRTC_SECURITY;
        if (scenario == 5) f.send_error = EWRTC_IO;
        assert(ewrtc_turn_start(t) == (scenario == 3 ? EWRTC_NOMEM : scenario == 4 ? EWRTC_SECURITY : EWRTC_IO));
        ewrtc_turn_destroy(t); assert(!f.live);
    }
}
static void test_pending_and_expiry(void) {
    fake f = {0}; ewrtc_turn_config c = config(&f); ewrtc_turn *t;
    assert(!ewrtc_turn_create(&c, &t));
    ewrtc_address a = {0xc0000202, 1000};
    assert(!ewrtc_turn_add_permission(t, &a) && !f.count);
    assert(!ewrtc_turn_start(t)); challenge(t, &f, 0, 401, STUN_ALLOCATE_ERROR);
    allocation_success(t, &f, 1, false);
    assert(f.count == 3 && ewrtc_read_u16(f.sent[2].bytes) == STUN_PERMISSION_REQUEST);
    f.now = 8000;
    assert(ewrtc_turn_tick(t) == EWRTC_TIMEOUT && !ewrtc_turn_has_permission(t, &a));
    ewrtc_turn_destroy(t); assert(!f.live);
    memset(&f, 0, sizeof(f)); t = ready(&f);
    f.now = 60000; assert(!ewrtc_turn_tick(t));
    challenge(t, &f, 2, 438, STUN_REFRESH_ERROR);
    allocation_success(t, &f, 3, true);
    f.now = 180000;
    assert(ewrtc_turn_tick(t) == EWRTC_TIMEOUT);
    ewrtc_turn_destroy(t); assert(!f.live);
}
int main(void) {
    test_permissions(); test_retry_and_failure(); test_pending_and_expiry();
    puts("TURN contracts passed");
    return 0;
}
