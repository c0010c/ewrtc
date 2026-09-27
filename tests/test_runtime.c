#define _POSIX_C_SOURCE 200809L
#include "ewrtc.h"
#include "ice/ice.h"
#include "ewrtc/platform/linux.h"
#include "stun/stun.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    atomic_size_t calls, fail_at, live;
} memory;
static void *alloc(void *ctx, size_t n) {
    memory *m = ctx;
    if (++m->calls == m->fail_at)
        return NULL;
    void *p = malloc(n);
    if (p)
        ++m->live;
    return p;
}
static void *resize(void *ctx, void *p, size_t n) {
    memory *m = ctx;
    if (++m->calls == m->fail_at)
        return NULL;
    bool fresh = !p;
    void *q = realloc(p, n);
    if (q && fresh)
        ++m->live;
    return q;
}
static void release(void *ctx, void *p) {
    memory *m = ctx;
    if (p) {
        assert(m->live);
        --m->live;
        free(p);
    }
}
static int fail_random(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)p;
    (void)n;
    return EWRTC_SECURITY;
}
static int fail_create(void *ctx, void **out) {
    (void)ctx;
    *out = NULL;
    return EWRTC_IO;
}
static int fail_thread(void *ctx, void *(*fn)(void *), void *arg, ewrtc_thread *out) {
    (void)ctx;
    (void)fn;
    (void)arg;
    *out = NULL;
    return EWRTC_IO;
}
static void creation_error(ewrtc_session *s, ewrtc_result error, const char *detail, void *user) {
    (void)s; (void)detail; atomic_store((atomic_int *)user, error);
}
static void test_session_failures(void) {
    for (size_t i = 1; i <= 3; ++i) {
        memory m = {.fail_at = i};
        ewrtc_context_config cc = {.pal = *ewrtc_pal_linux()};
        cc.pal.memory = (ewrtc_allocator){&m, alloc, resize, release};
        ewrtc_context *context = (void *)1;
        assert(ewrtc_context_create(&cc, &context) == EWRTC_NOMEM && !context && !m.live);
    }
    for (size_t i = 1; i <= 6; ++i) {
        memory m = {0};
        ewrtc_context_config cc = {.pal = *ewrtc_pal_linux()};
        cc.pal.memory = (ewrtc_allocator){&m, alloc, resize, release};
        ewrtc_context *context;
        assert(!ewrtc_context_create(&cc, &context));
        size_t baseline = m.live;
        m.fail_at = m.calls + i;
        ewrtc_session_config c; ewrtc_session_config_init(&c);
        c.stun_host = c.turn_host = "localhost";
        c.turn_username = "test"; c.turn_password = "pass";
        ewrtc_callbacks cb = {0}; ewrtc_session *session = (void *)1;
        assert(ewrtc_session_create(context, &c, &cb, NULL, &session) == EWRTC_NOMEM);
        assert(!session && m.live == baseline);
        assert(!ewrtc_context_destroy(context) && !m.live);
    }
    for (unsigned i = 0; i < 4; ++i) {
        memory m = {0};
        ewrtc_context_config cc = {.pal = *ewrtc_pal_linux()};
        cc.pal.memory = (ewrtc_allocator){&m, alloc, resize, release};
        if (i == 0) cc.pal.random.bytes = fail_random;
        if (i == 1) cc.pal.threads.mutex_create = fail_create;
        if (i == 2) cc.pal.threads.condition_create = fail_create;
        if (i == 3) cc.pal.threads.thread_create = fail_thread;
        ewrtc_context *context = NULL;
        int result = ewrtc_context_create(&cc, &context);
        if (i) assert(result == EWRTC_IO && !context);
        else {
            assert(!result);
            ewrtc_session_config c; ewrtc_session_config_init(&c);
            atomic_int error = 0;
            ewrtc_callbacks cb = {.on_error = creation_error}; ewrtc_session *session;
            assert(!ewrtc_session_create(context, &c, &cb, &error, &session));
            assert(!ewrtc_session_create_offer(session));
            for (unsigned tick = 0; tick < 2000 && !atomic_load(&error); ++tick) {
                struct timespec t = {0, 1000000}; nanosleep(&t, NULL);
            }
            assert(error == EWRTC_SECURITY);
            assert(!ewrtc_session_destroy(session) && !ewrtc_context_destroy(context));
        }
        assert(!m.live);
    }
}
typedef struct {
    uint64_t mono, utc;
    bool fail_open, fail_dns;
    unsigned opened, closed, received, sent;
} network;
static uint64_t mono(void *ctx) {
    return ((network *)ctx)->mono;
}
static uint64_t utc(void *ctx) {
    return ((network *)ctx)->utc;
}
static int udp_open(void *ctx, const ewrtc_address *a, ewrtc_socket *s) {
    (void)a;
    network *n = ctx;
    if (n->fail_open)
        return EWRTC_IO;
    ++n->opened;
    *s = n;
    return 0;
}
static void udp_close(void *ctx, ewrtc_socket s) {
    network *n = ctx;
    assert(s == n);
    ++n->closed;
}
static int udp_local(void *ctx, ewrtc_socket s, ewrtc_address *a) {
    assert(ctx == s);
    *a = (ewrtc_address){0, 12345};
    return 0;
}
static int udp_send(void *ctx, ewrtc_socket s, const ewrtc_address *a, const void *p, size_t n) {
    (void)a;
    (void)p;
    (void)n;
    assert(ctx == s);
    ++((network *)ctx)->sent;
    return 0;
}
static int udp_receive(void *ctx, ewrtc_socket s, ewrtc_address *a, void *p, size_t cap,
                       size_t *n) {
    (void)a;
    (void)p;
    (void)cap;
    assert(ctx == s);
    ++((network *)ctx)->received;
    *n = 0;
    return EWRTC_AGAIN;
}
static int resolve(void *ctx, const char *s, uint16_t port, ewrtc_address *a) {
    (void)s;
    if (((network *)ctx)->fail_dns)
        return EWRTC_IO;
    *a = (ewrtc_address){0x7f000001, port};
    return 0;
}
static int interfaces(void *ctx, ewrtc_address *a, size_t cap, size_t *n) {
    (void)ctx;
    assert(cap);
    *a = (ewrtc_address){0xc0000201, 0};
    *n = 1;
    return 0;
}
typedef struct {
    pthread_t owner;
    unsigned states, candidates, done;
    ewrtc_state last;
} events;
static void state(void *ctx, ewrtc_state state) {
    events *e = ctx;
    assert(pthread_equal(e->owner, pthread_self()));
    ++e->states;
    e->last = state;
}
static void candidate(void *ctx, const char *s) {
    events *e = ctx;
    assert(pthread_equal(e->owner, pthread_self()));
    assert(strstr(s, "candidate:"));
    ++e->candidates;
}
static void done(void *ctx) {
    events *e = ctx;
    assert(pthread_equal(e->owner, pthread_self()));
    ++e->done;
}
static void receive(void *ctx, const uint8_t *p, size_t n) {
    events *e = ctx;
    (void)p;
    (void)n;
    assert(pthread_equal(e->owner, pthread_self()));
}
static void test_ice(void) {
#if EWRTC_WITH_NATIVE_ICE
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        network net = {.mono = 0};
        memory mem = {0};
        events e = {.owner = pthread_self()};
        ewrtc_ice_config c = {.pal = *ewrtc_pal_linux(),
                              .backend = EWRTC_ICE_NATIVE,
                              .crypto_backend =
                                  EWRTC_WITH_OPENSSL ? EWRTC_CRYPTO_OPENSSL : EWRTC_CRYPTO_MBEDTLS};
        c.pal.memory = (ewrtc_allocator){&mem, alloc, resize, release};
        c.pal.network = (ewrtc_network){&net,     udp_open,    udp_close, udp_local,
                                        udp_send, udp_receive, resolve,   interfaces};
        c.pal.clock = (ewrtc_clock){&net, mono, utc};
        if (scenario == 1) {
            net.fail_dns = true;
            c.stun_host = "bad-host";
        }
        if (scenario == 2)
            net.fail_open = true;
        if (scenario == 3)
            c.pal.random.bytes = fail_random;
        ewrtc_ice_events callbacks = {state, candidate, done, receive, &e};
        ewrtc_ice *ice;
        int r = ewrtc_ice_create(&c, &callbacks, &ice);
        if (scenario) {
            assert(r && !ice && !mem.live && net.opened == net.closed);
            continue;
        }
        assert(!r && !e.states);
        assert(ewrtc_ice_next_deadline(ice) == UINT64_MAX);
        assert(!ewrtc_ice_start(ice));
        assert(e.candidates == 1 && e.done == 1 && e.last == EWRTC_CONNECTING);
        assert(ewrtc_ice_start(ice) == EWRTC_STATE);
        assert(ewrtc_ice_next_deadline(ice) == 30000);
        assert(!ewrtc_ice_tick(ice) && net.received == 1);
        net.utc = 999999999999ULL;
        assert(!ewrtc_ice_tick(ice) && e.last != EWRTC_FAILED);
        net.mono += 30000;
        assert(!ewrtc_ice_tick(ice) && e.last == EWRTC_FAILED);
        assert(ewrtc_ice_next_deadline(ice) == UINT64_MAX);
        ewrtc_ice_destroy(ice);
        assert(net.opened == net.closed && !mem.live);
    }
#endif
#if EWRTC_WITH_LIBJUICE
    events e = {.owner = pthread_self()};
    ewrtc_ice_config c = {.pal = *ewrtc_pal_linux(), .backend = EWRTC_ICE_LIBJUICE};
    ewrtc_ice_events cb = {state, candidate, done, receive, &e};
    ewrtc_ice *ice;
    assert(!ewrtc_ice_create(&c, &cb, &ice));
    assert(!e.states && !e.candidates);
    assert(!ewrtc_ice_start(ice));
    for (unsigned i = 0; i < 200 && !e.done; ++i) {
        assert(!ewrtc_ice_tick(ice));
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    assert(e.done && e.candidates);
    ewrtc_ice_destroy(ice);
    unsigned total = e.states + e.candidates + e.done;
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
    assert(total == e.states + e.candidates + e.done);
#endif
}
static atomic_int callback_count;
static void session_state(ewrtc_session *s, ewrtc_state state, void *ctx) {
    (void)state;
    (void)ctx;
    ewrtc_stats stats;
    assert(!ewrtc_session_get_stats(s, &stats));
    assert(ewrtc_session_destroy(s) == EWRTC_STATE);
    atomic_fetch_add(&callback_count, 1);
}
static void test_session_callbacks(void) {
    ewrtc_session_config c; ewrtc_session_config_init(&c);
    ewrtc_context_config cc = {.pal = *ewrtc_pal_linux()}; ewrtc_context *context;
    assert(!ewrtc_context_create(&cc, &context));
    ewrtc_callbacks cb = {.on_state = session_state};
    ewrtc_session *s;
    assert(!ewrtc_session_create(context, &c, &cb, NULL, &s));
    assert(!ewrtc_session_set_remote_offer(s, "invalid"));
    for (unsigned i = 0; i < 1000 && !atomic_load(&callback_count); ++i) {
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    assert(atomic_load(&callback_count));
    assert(!ewrtc_session_destroy(s));
    assert(!ewrtc_context_destroy(context));
    int n = atomic_load(&callback_count);
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
    assert(n == atomic_load(&callback_count));
}
static void test_linux_pal(void) {
    const ewrtc_pal *p = ewrtc_pal_linux();
    ewrtc_mutex m;
    ewrtc_condition c;
    assert(!p->threads.mutex_create(p->threads.ctx, &m));
    assert(!p->threads.condition_create(p->threads.ctx, &c));
    p->threads.mutex_lock(p->threads.ctx, m);
    uint64_t start = ewrtc_now_ms(p);
    assert(p->threads.condition_wait(p->threads.ctx, c, m, 5) == EWRTC_TIMEOUT);
    assert(ewrtc_now_ms(p) >= start + 4);
    p->threads.mutex_unlock(p->threads.ctx, m);
    p->threads.condition_destroy(p->threads.ctx, c);
    p->threads.mutex_destroy(p->threads.ctx, m);
    ewrtc_address addr = {0x7f000001, 0};
    ewrtc_socket a, b;
    assert(!p->network.udp_open(p->network.ctx, &addr, &a));
    assert(!p->network.udp_open(p->network.ctx, &addr, &b));
    ewrtc_address destination;
    assert(!p->network.udp_local(p->network.ctx, b, &destination));
    uint8_t bytes[32] = {0};
    size_t n;
    assert(p->network.udp_receive(p->network.ctx, b, &addr, bytes, sizeof(bytes), &n) ==
           EWRTC_AGAIN);
    assert(!p->network.udp_send(p->network.ctx, a, &destination, "hello", 5));
    int r = EWRTC_AGAIN;
    for (unsigned i = 0; i < 100 && r == EWRTC_AGAIN; ++i)
        r = p->network.udp_receive(p->network.ctx, b, &addr, bytes, sizeof(bytes), &n);
    assert(!r && n == 5 && !memcmp(bytes, "hello", 5));
    p->network.udp_close(p->network.ctx, a);
    p->network.udp_close(p->network.ctx, b);
}
#include "offer.h"
typedef struct { atomic_bool closed, answered; int errors; ewrtc_result error; } delivery_test;
static void delivery_answer(ewrtc_session *s, const char *answer, void *ctx) {
    (void)answer;
    delivery_test *d = ctx;
    unsigned count = 0;
    while (ewrtc_session_end_remote_candidates(s) == EWRTC_OK) assert(++count <= 4096);
    atomic_store(&d->answered, true);
}
static void delivery_error(ewrtc_session *s, ewrtc_result error, const char *message, void *ctx) {
    (void)s; (void)message;
    delivery_test *d = ctx; d->error = error; ++d->errors;
}
static void delivery_state(ewrtc_session *s, ewrtc_state state, void *ctx) {
    (void)s;
    if (state == EWRTC_CLOSED) atomic_store(&((delivery_test *)ctx)->closed, true);
}
static void test_internal_delivery(void) {
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        delivery_test d = {0};
        ewrtc_context_config cc = {.pal = *ewrtc_pal_linux(), .control_slots = scenario ? 1 : 64};
        ewrtc_context *context; assert(!ewrtc_context_create(&cc, &context));
        ewrtc_session_config c; ewrtc_session_config_init(&c);
        ewrtc_callbacks cb = {.on_local_sdp = delivery_answer, .on_error = delivery_error,
                              .on_state = delivery_state};
        ewrtc_session *s; assert(!ewrtc_session_create(context, &c, &cb, &d, &s));
        assert(!ewrtc_session_set_remote_offer(s, test_offer));
        for (unsigned i = 0; i < 2000 && !atomic_load(&d.answered); ++i) {
            struct timespec t = {0, 1000000}; nanosleep(&t, NULL);
        }
        assert(atomic_load(&d.answered));
        if (scenario) {
            for (unsigned i = 0; i < 2000 && !atomic_load(&d.closed); ++i) {
                struct timespec t = {0, 1000000}; nanosleep(&t, NULL);
            }
            assert(atomic_load(&d.closed));
        }
        assert(!ewrtc_session_destroy(s));
        if (scenario) assert(d.error == EWRTC_BACKPRESSURE && d.errors == 1);
        else assert(!d.errors); /* A full ordinary queue cannot steal control slots. */
        ewrtc_context_stats stats; assert(!ewrtc_context_get_stats(context, &stats));
        assert(!stats.queue_bytes && !stats.control_reserved_bytes && !stats.sessions);
        assert(!ewrtc_context_destroy(context));
    }
}
int main(void) {
    test_linux_pal();
    test_session_failures();
    test_ice();
    test_session_callbacks();
    test_internal_delivery();
    puts("PAL, ICE dispatch and session failure/lifecycle tests passed");
    return 0;
}
