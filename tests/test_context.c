#define _GNU_SOURCE
#include "../src/session/private.h" /* fault injection at the runtime admission boundary */
#include "ewrtc/platform/linux.h"
#include <assert.h>
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/* Track every PAL mutex, including the libjuice adapter's outer delivery lock. */
static _Thread_local unsigned log_lock_depth;
static void log_test_lock(void *ctx, ewrtc_mutex mutex) {
    (void)ctx;
    ewrtc_pal_linux()->threads.mutex_lock(NULL, mutex);
    ++log_lock_depth;
}
static void log_test_unlock(void *ctx, ewrtc_mutex mutex) {
    (void)ctx;
    assert(log_lock_depth);
    --log_lock_depth;
    ewrtc_pal_linux()->threads.mutex_unlock(NULL, mutex);
}
static void checked_log(void *ctx, int level, const char *text) {
    (void)ctx;
    assert(!log_lock_depth);
    ewrtc_pal_linux()->log.write(NULL, level, text);
}
static void pause_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL);
}
static atomic_bool dns_entered, dns_release;
/* Interpose only our test hostname, including calls made by libjuice's resolver.
 * No dependency on a real slow resolver or an external DNS service. */
int getaddrinfo(const char *host, const char *service, const struct addrinfo *hint, struct addrinfo **out) {
    if (host && !strcmp(host, "missing.ewrtc.invalid")) { *out = NULL; return EAI_NONAME; }
    if (host && !strcmp(host, "slow.ewrtc.invalid") && (!hint || !(hint->ai_flags & AI_NUMERICHOST))) {
        atomic_store(&dns_entered, true);
        while (!atomic_load(&dns_release)) pause_ms(1);
        *out = NULL; return EAI_NONAME;
    }
    typedef int (*resolve_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    resolve_fn real;
    void *symbol = dlsym(RTLD_NEXT, "getaddrinfo");
    memcpy(&real, &symbol, sizeof(real)); assert(real);
    return real(host, service, hint, out);
}
typedef struct {
    atomic_size_t allocations, live, threads, waits, infinite_waits;
    atomic_bool fail_next;
    size_t thread_attempts, event_attempts, fail_thread_at, fail_event_at;
    bool fail_add;
} services;
static void *test_alloc(void *user, size_t n) {
    services *s = user;
    ++s->allocations;
    if (atomic_exchange(&s->fail_next, false)) return NULL;
    void *p = malloc(n); if (p) ++s->live; return p;
}
static void *test_resize(void *user, void *p, size_t n) {
    services *s = user; bool fresh = !p;
    ++s->allocations;
    if (atomic_exchange(&s->fail_next, false)) return NULL;
    void *q = realloc(p, n); if (q && fresh) ++s->live; return q;
}
static void test_free(void *user, void *p) {
    if (p) { --((services *)user)->live; free(p); }
}
static int test_thread(void *user, void *(*fn)(void *), void *arg, ewrtc_thread *out) {
    services *s = user;
    if (++s->thread_attempts == s->fail_thread_at) { *out = NULL; return EWRTC_IO; }
    int r = ewrtc_pal_linux()->threads.thread_create(NULL, fn, arg, out);
    if (!r) ++s->threads; return r;
}
static int test_join(void *user, ewrtc_thread t) {
    int r = ewrtc_pal_linux()->threads.thread_join(NULL, t);
    if (!r) --((services *)user)->threads; return r;
}
static int test_wait(void *user, ewrtc_waiter w, uint64_t *tokens, size_t cap, size_t *count, uint32_t ms) {
    services *s = user; ++s->waits; if (ms == UINT32_MAX) ++s->infinite_waits;
    return ewrtc_pal_linux()->events.wait(NULL, w, tokens, cap, count, ms);
}
static int test_event_create(void *user, ewrtc_waiter *out) {
    services *s = user;
    if (++s->event_attempts == s->fail_event_at) { *out = NULL; return EWRTC_IO; }
    return ewrtc_pal_linux()->events.create(NULL, out);
}
static int test_event_add(void *user, ewrtc_waiter w, ewrtc_socket socket, uint64_t token) {
    if (((services *)user)->fail_add) return EWRTC_IO;
    return ewrtc_pal_linux()->events.add(NULL, w, socket, token);
}
static ewrtc_context_config config(services *s) {
    ewrtc_context_config c; ewrtc_context_config_init(&c); c.pal = *ewrtc_pal_linux();
    c.pal.memory = (ewrtc_allocator){s, test_alloc, test_resize, test_free};
    c.pal.threads.ctx = s; c.pal.threads.thread_create = test_thread; c.pal.threads.thread_join = test_join;
    c.pal.threads.mutex_lock = log_test_lock; c.pal.threads.mutex_unlock = log_test_unlock;
    c.pal.log = (ewrtc_logger){NULL, checked_log};
    c.pal.events.ctx = s; c.pal.events.wait = test_wait;
    c.pal.events.create = test_event_create; c.pal.events.add = test_event_add;
    return c;
}
typedef struct peer {
    ewrtc_session *session;
    struct peer *remote;
    bool offerer;
    atomic_uint connected, closed, errors, video, audio, done, states;
    atomic_int last_error;
    pthread_t owner;
    bool owner_set;
} peer;
static void check_owner(peer *p) {
    if (!p->owner_set) { p->owner = pthread_self(); p->owner_set = true; }
    assert(pthread_equal(p->owner, pthread_self()));
}
static void state(ewrtc_session *s, ewrtc_state value, void *user) {
    peer *p = user; check_owner(p);
    assert(ewrtc_session_destroy(s) == EWRTC_STATE);
    assert(ewrtc_context_destroy(s->context) == EWRTC_STATE);
    if (p->remote && p->remote->session) assert(ewrtc_session_destroy(p->remote->session) == EWRTC_STATE);
    ++p->states;
    if (value == EWRTC_CONNECTED) atomic_store(&p->connected, 1);
    if (value == EWRTC_CLOSED) ++p->closed;
}
static void error(ewrtc_session *s, ewrtc_result value, const char *detail, void *user) {
    (void)s; (void)detail; peer *p = user; check_owner(p); p->last_error = value; ++p->errors;
}
static void sdp(ewrtc_session *s, const char *text, void *user) {
    (void)s; peer *p = user; check_owner(p);
    if (!p->remote) return;
    int r = p->offerer ? ewrtc_session_set_remote_offer(p->remote->session, text) :
                        ewrtc_session_set_remote_answer(p->remote->session, text);
    assert(!r);
}
static void candidate(ewrtc_session *s, const char *text, void *user) {
    (void)s; peer *p = user; check_owner(p);
    if (p->remote) assert(!ewrtc_session_add_remote_candidate(p->remote->session, text));
}
static void done(ewrtc_session *s, void *user) {
    (void)s; peer *p = user; check_owner(p);
    if (p->remote) assert(!ewrtc_session_end_remote_candidates(p->remote->session));
    ++p->done;
}
static void video(ewrtc_session *s, const uint8_t *data, size_t n, uint32_t ts, int key, void *user) {
    (void)s; (void)ts; peer *p = user; check_owner(p);
    assert(n == 16384 && data[4] == 0x67 && key); ++p->video;
}
static void audio(ewrtc_session *s, const uint8_t *data, size_t n, uint32_t ts, uint16_t seq, void *user) {
    (void)s; (void)ts; (void)seq; peer *p = user; check_owner(p);
    assert(n == 3 && data[0] == 0xf8); ++p->audio;
}
static const ewrtc_callbacks callbacks = {.on_state = state, .on_error = error, .on_local_sdp = sdp,
    .on_local_candidate = candidate, .on_gathering_done = done, .on_video = video, .on_audio = audio};
static void connect_pairs(ewrtc_context *send, ewrtc_context *receive, peer *a, peer *b, size_t n, int ice) {
    ewrtc_session_config c; ewrtc_session_config_init(&c); c.ice_backend = ice;
    for (size_t i = 0; i < n; ++i) {
        a[i].offerer = true; a[i].remote = &b[i]; b[i].remote = &a[i];
        c.video_direction = c.audio_direction = EWRTC_SENDONLY;
        assert(!ewrtc_session_create(send, &c, &callbacks, &a[i], &a[i].session));
        c.video_direction = c.audio_direction = EWRTC_RECVONLY;
        assert(!ewrtc_session_create(receive, &c, &callbacks, &b[i], &b[i].session));
    }
    for (size_t i = 0; i < n; ++i) assert(!ewrtc_session_create_offer(a[i].session));
    for (unsigned step = 0; step < 5000; ++step) {
        bool complete = true;
        for (size_t i = 0; i < n; ++i) {
            assert(!a[i].errors && !b[i].errors);
            if (!a[i].connected || !b[i].connected || !a[i].done || !b[i].done) complete = false;
        }
        if (complete) return;
        pause_ms(1);
    }
    assert(!"pair connection timeout");
}
static uint8_t frame[16384];
static const uint8_t opus[] = {0xf8, 0xff, 0xfe};
static void send_round(peer *a, size_t n, unsigned round) {
    for (size_t i = 0; i < n; ++i) {
        assert(!ewrtc_session_send_video(a[i].session, frame, sizeof(frame), (uint64_t)round * 33333, 1));
        assert(!ewrtc_session_send_audio(a[i].session, opus, sizeof(opus), (uint64_t)round * 20000));
    }
}
static void destroy_pair(peer *a, peer *b) {
    assert(!ewrtc_session_close(a->session)); assert(!ewrtc_session_close(b->session));
    assert(!ewrtc_session_destroy(a->session)); assert(!ewrtc_session_destroy(b->session));
    a->session = b->session = NULL;
    assert(a->closed == 1 && b->closed == 1);
}
static void test_streams(size_t n, size_t workers, int ice) {
    services sa = {0}, sb = {0};
    ewrtc_context_config ca = config(&sa), cb = config(&sb);
    ca.worker_count = cb.worker_count = workers;
    ewrtc_context *send, *receive;
    assert(!ewrtc_context_create(&ca, &send) && !ewrtc_context_create(&cb, &receive));
    peer a[8] = {0}, b[8] = {0}; connect_pairs(send, receive, a, b, n, ice);
    assert(sa.threads == workers + 1 && sb.threads == workers + 1);
    assert(ewrtc_context_destroy(receive) == EWRTC_STATE);
    unsigned rounds = n == 8 ? 150 : 15;
    for (unsigned r = 0; r < rounds; ++r) { send_round(a, n, r); pause_ms(33); }
    pause_ms(100);
    for (size_t i = 0; i < n; ++i) assert(b[i].video >= rounds * 8 / 10 && b[i].audio >= rounds * 8 / 10 && !b[i].errors);
    /* Five fragmented AUs exceed the 64-datagram quantum while fitting the
     * loopback socket receive buffer. Require the tail to drain without new input. */
    unsigned counts[8];
    for (size_t i = 0; i < n; ++i) counts[i] = b[i].video;
    for (unsigned r = rounds; r < rounds + 5; ++r) send_round(a, n, r);
    pause_ms(300);
    for (size_t i = 0; i < n; ++i) assert(b[i].video >= counts[i] + 5 && !b[i].errors);
    rounds += 5;
    /* Remove and replace one route while the others keep receiving. */
    destroy_pair(&a[0], &b[0]);
    unsigned before = b[1].video;
    for (unsigned r = rounds; r < rounds + 5; ++r) { send_round(a + 1, n - 1, r); pause_ms(33); }
    assert(b[1].video > before);
    memset(&a[0], 0, sizeof(a[0])); memset(&b[0], 0, sizeof(b[0]));
    connect_pairs(send, receive, a, b, 1, ice);
    send_round(a, 1, 0); pause_ms(100); assert(b[0].video);
    ewrtc_context_stats stats; assert(!ewrtc_context_get_stats(receive, &stats));
    struct rusage usage; getrusage(RUSAGE_SELF, &usage);
    uint64_t dropped = 0;
    for (size_t i = 0; i < n; ++i) {
        ewrtc_stats v; assert(!ewrtc_session_get_stats(b[i].session, &v)); dropped += v.dropped_video_frames;
        destroy_pair(&a[i], &b[i]);
    }
    printf("streams=%zu workers=%zu ICE=%d peak_queue=%zu reserved=%zu drops=%llu wait/timer/callback/turn_ms=%llu/%llu/%llu/%llu rss_kib=%ld cpu_s=%.3f\n",
        n, workers, ice, stats.peak_queue_bytes, stats.control_reserved_bytes, (unsigned long long)dropped,
        (unsigned long long)stats.max_schedule_wait_ms, (unsigned long long)stats.max_timer_delay_ms,
        (unsigned long long)stats.max_callback_ms, (unsigned long long)stats.max_turn_ms, usage.ru_maxrss,
        usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6);
    assert(!ewrtc_context_get_stats(receive, &stats));
    assert(!stats.sessions && !stats.queue_bytes && !stats.control_reserved_bytes);
    assert(!ewrtc_context_destroy(send) && !ewrtc_context_destroy(receive));
    assert(!sa.live && !sb.live && !sa.threads && !sb.threads);
}
static void test_idle_and_control(void) {
    services service = {0}; ewrtc_context_config cc = config(&service);
    cc.control_slots = 4; cc.max_sessions = 2;
    cc.queue_limit_bytes = 8 * sizeof(control_slot) + 4096;
    ewrtc_context *context; assert(!ewrtc_context_create(&cc, &context));
    ewrtc_session_config c; ewrtc_session_config_init(&c);
    peer a = {0}, b = {0};
    assert(!ewrtc_session_create(context, &c, &callbacks, &a, &a.session));
    assert(!ewrtc_session_create(context, &c, &callbacks, &b, &b.session));
    pause_ms(20); size_t waits = service.waits;
    pause_ms(50); assert(service.waits == waits && service.infinite_waits);
    int error_code;
    void *full = session_allocate(a.session, 4096 - sizeof(queue_header), false, &error_code);
    assert(full && !error_code);
    assert(!session_delivery_allocate(a.session, 20, false, &error_code) && error_code == EWRTC_BACKPRESSURE);
    /* A different session's control and close paths remain independently funded. */
    session_internal_event(b.session, WORK_ICE_STATE, NULL, 0, EWRTC_GATHERING);
    for (unsigned i = 0; i < 250 && !b.states; ++i) pause_ms(1);
    assert(b.states && !b.errors);
    ewrtc_stats peer_stats; assert(!ewrtc_session_get_stats(b.session, &peer_stats));
    assert(peer_stats.state == EWRTC_GATHERING);
    assert(!ewrtc_session_destroy(b.session)); b.session = NULL;
    assert(b.closed == 1 && !b.errors);
    session_release(a.session, full);
    assert(!ewrtc_session_destroy(a.session)); a.session = NULL;
    ewrtc_context_stats stats; assert(!ewrtc_context_get_stats(context, &stats));
    assert(stats.congestion_drops == 1 && !stats.queue_bytes);
    assert(!ewrtc_context_destroy(context) && !service.live);
    puts("idle wait, control reservation, overload drop and close passed");
}
typedef struct { ewrtc_session *session; atomic_bool done; } destroying;
static void *destroy_main(void *arg) {
    destroying *d = arg; assert(!ewrtc_session_destroy(d->session)); atomic_store(&d->done, true); return NULL;
}
static void test_slow_close(int ice) {
    services sa = {0}, sb = {0}; ewrtc_context_config ca = config(&sa), cb = config(&sb);
    ewrtc_context *send, *receive;
    assert(!ewrtc_context_create(&ca, &send) && !ewrtc_context_create(&cb, &receive));
    peer a[2] = {0}, b[2] = {0}; connect_pairs(send, receive, a, b, 2, ice);
    ewrtc_session_config c; ewrtc_session_config_init(&c); c.ice_backend = ice; c.stun_host = "slow.ewrtc.invalid";
    peer slow = {0}; assert(!ewrtc_session_create(receive, &c, &callbacks, &slow, &slow.session));
    dns_entered = false; dns_release = false;
    assert(!ewrtc_session_create_offer(slow.session));
    for (unsigned i = 0; i < 3000 && !dns_entered; ++i) pause_ms(1);
    assert(dns_entered);
    destroying d = {.session = slow.session}; pthread_t destroyer;
    assert(!pthread_create(&destroyer, NULL, destroy_main, &d));
    for (unsigned r = 0; r < 10; ++r) { send_round(a, 2, r); pause_ms(20); }
    fprintf(stderr, "slow close: received=%u/%u destroy_done=%d closed=%u\n", (unsigned)b[0].video, (unsigned)b[1].video, (int)d.done, (unsigned)slow.closed);
    assert(b[0].video && b[1].video && !d.done && !slow.closed);
    dns_release = true; assert(!pthread_join(destroyer, NULL));
    assert(d.done && slow.closed == 1);
    for (unsigned i = 0; i < 2; ++i) destroy_pair(&a[i], &b[i]);
    assert(!ewrtc_context_destroy(send) && !ewrtc_context_destroy(receive));
    assert(!sa.live && !sb.live);
    printf("slow DNS close ICE=%d leaves existing I/O running\n", ice);
}
static void test_candidate_dns_failure(int ice) {
    services sa = {0}, sb = {0}; ewrtc_context_config ca = config(&sa), cb = config(&sb);
    ewrtc_context *send, *receive;
    assert(!ewrtc_context_create(&ca, &send) && !ewrtc_context_create(&cb, &receive));
    peer a[1] = {0}, b[1] = {0}; connect_pairs(send, receive, a, b, 1, ice);
    assert(!ewrtc_session_add_remote_candidate(b[0].session,
        "candidate:missing 1 UDP 1 missing.ewrtc.invalid 1234 typ host"));
    for (unsigned i = 0; i < 2000 && !b[0].errors; ++i) pause_ms(1);
    assert(b[0].errors == 1 && b[0].last_error == EWRTC_INVALID && !b[0].closed);
    send_round(a, 1, 0); pause_ms(100);
    assert(b[0].video && b[0].audio);
    destroy_pair(a, b);
    assert(!ewrtc_context_destroy(send) && !ewrtc_context_destroy(receive));
    assert(!sa.live && !sb.live);
    printf("candidate DNS rejection ICE=%d preserves the established stream\n", ice);
}
static void test_juice_error_wake(void) {
#if EWRTC_WITH_LIBJUICE
    services sa = {0}, sb = {0}; ewrtc_context_config ca = config(&sa), cb = config(&sb);
    ewrtc_context *send, *receive;
    assert(!ewrtc_context_create(&ca, &send) && !ewrtc_context_create(&cb, &receive));
    peer a[1] = {0}, b[1] = {0}; connect_pairs(send, receive, a, b, 1, EWRTC_ICE_LIBJUICE);
    pause_ms(50);
    sb.fail_next = true;
    uint64_t at = ewrtc_now_ms(&cb.pal);
    assert(!ewrtc_session_send_audio(a[0].session, opus, sizeof(opus), 0));
    while (!b[0].closed && ewrtc_now_ms(&cb.pal) - at < 250) pause_ms(1);
    assert(b[0].closed && b[0].last_error == EWRTC_NOMEM);
    destroy_pair(a, b);
    assert(!ewrtc_context_destroy(send) && !ewrtc_context_destroy(receive));
    assert(!sa.live && !sb.live);
    puts("sleeping libjuice worker wakes on first failed media allocation");
#endif
}
static void test_juice_media_pressure(void) {
#if EWRTC_WITH_LIBJUICE
    services sa = {0}, sb = {0}; ewrtc_context_config ca = config(&sa), cb = config(&sb);
    cb.queue_limit_bytes = 256 * 1024;
    ewrtc_context *send, *receive;
    assert(!ewrtc_context_create(&ca, &send) && !ewrtc_context_create(&cb, &receive));
    peer a[1] = {0}, b[1] = {0}; connect_pairs(send, receive, a, b, 1, EWRTC_ICE_LIBJUICE);
    pause_ms(50);
    ewrtc_context_stats stats; assert(!ewrtc_context_get_stats(receive, &stats));
    assert(!stats.queue_bytes);
    int result;
    void *full = session_allocate(b[0].session,
        cb.queue_limit_bytes - stats.control_reserved_bytes - sizeof(queue_header), false, &result);
    assert(full);
    send_round(a, 1, 0); pause_ms(100);
    assert(!ewrtc_context_get_stats(receive, &stats));
    assert(stats.congestion_drops && !b[0].errors && !b[0].closed);
    session_release(b[0].session, full);
    send_round(a, 1, 1); pause_ms(200);
    assert(b[0].video && b[0].audio && !b[0].errors);
    destroy_pair(a, b);
    assert(!ewrtc_context_destroy(send) && !ewrtc_context_destroy(receive));
    assert(!sa.live && !sb.live);
    puts("libjuice media pressure drops recover without closing the route");
#endif
}
static void test_partial_construction(void) {
    for (size_t at = 1; at <= 3; ++at) {
        services service = {.fail_thread_at = at}; ewrtc_context_config cc = config(&service);
        cc.worker_count = 2;
        ewrtc_context *context = (void *)1;
        assert(ewrtc_context_create(&cc, &context) == EWRTC_IO && !context);
        assert(!service.live && !service.threads);
    }
    for (size_t at = 1; at <= 2; ++at) {
        services service = {.fail_event_at = at}; ewrtc_context_config cc = config(&service);
        cc.worker_count = 2;
        ewrtc_context *context = (void *)1;
        assert(ewrtc_context_create(&cc, &context) == EWRTC_IO && !context);
        assert(!service.live && !service.threads);
    }
#if EWRTC_WITH_NATIVE_ICE
    services service = {.fail_add = true}; ewrtc_context_config cc = config(&service);
    ewrtc_context *context; assert(!ewrtc_context_create(&cc, &context));
    ewrtc_session_config cfg; ewrtc_session_config_init(&cfg);
    peer p = {0}; assert(!ewrtc_session_create(context, &cfg, &callbacks, &p, &p.session));
    assert(!ewrtc_session_create_offer(p.session));
    for (unsigned i = 0; i < 2000 && !p.closed; ++i) pause_ms(1);
    assert(p.closed && p.errors == 1 && p.last_error == EWRTC_IO);
    assert(!ewrtc_session_destroy(p.session) && !ewrtc_context_destroy(context));
    assert(!service.live && !service.threads);
#endif
    const ewrtc_pal *pal = ewrtc_pal_linux(); ewrtc_waiter waiter;
    assert(!pal->events.create(NULL, &waiter));
    assert(!pal->events.wake(NULL, waiter));
    size_t count; uint64_t tokens[4], at = ewrtc_now_ms(pal);
    assert(!pal->events.wait(NULL, waiter, tokens, 4, &count, 500));
    assert(!count && ewrtc_now_ms(pal) - at < 100);
    pal->events.destroy(NULL, waiter);
    puts("partial worker/waiter construction, registration failure and sticky wake passed");
}
static atomic_bool closed_entered, closed_release;
static void slow_closed(ewrtc_session *session, ewrtc_state value, void *user) {
    (void)session; (void)user;
    if (value == EWRTC_CLOSED) {
        closed_entered = true;
        while (!closed_release) pause_ms(1);
    }
}
static void test_destroy_barrier(void) {
    services service = {0}; ewrtc_context_config cc = config(&service); ewrtc_context *context;
    assert(!ewrtc_context_create(&cc, &context));
    ewrtc_session_config cfg; ewrtc_session_config_init(&cfg);
    ewrtc_callbacks cb = {.on_state = slow_closed}; ewrtc_session *session;
    assert(!ewrtc_session_create(context, &cfg, &cb, NULL, &session));
    destroying d = {.session = session}; pthread_t destroyer;
    closed_entered = closed_release = false;
    assert(!pthread_create(&destroyer, NULL, destroy_main, &d));
    for (unsigned i = 0; i < 2000 && !closed_entered; ++i) pause_ms(1);
    assert(closed_entered && !d.done);
    assert(ewrtc_context_destroy(context) == EWRTC_STATE);
    closed_release = true; assert(!pthread_join(destroyer, NULL) && d.done);
    assert(!ewrtc_context_destroy(context) && !service.live);
    puts("destroy waits for final owner-thread CLOSED callback");
}
static void *churn(void *arg) {
    ewrtc_context *context = arg;
    ewrtc_session_config cfg; ewrtc_session_config_init(&cfg); ewrtc_callbacks cb = {0};
    for (unsigned i = 0; i < 50; ++i) {
        ewrtc_session *session;
        assert(!ewrtc_session_create(context, &cfg, &cb, NULL, &session));
        assert(!ewrtc_session_destroy(session));
    }
    return NULL;
}
static void test_concurrent_admission(void) {
    services service = {0}; ewrtc_context_config cc = config(&service); cc.worker_count = 2;
    ewrtc_context *context; assert(!ewrtc_context_create(&cc, &context));
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, churn, context));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    ewrtc_context_stats stats; assert(!ewrtc_context_get_stats(context, &stats));
    assert(!stats.sessions && !stats.control_reserved_bytes && !stats.queue_bytes && service.threads == 3);
    assert(!ewrtc_context_destroy(context) && !service.live);
    puts("concurrent session admission and destruction passed");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint8_t header[] = {0,0,0,1,0x67,1,0,0,0,1,0x68,2,0,0,0,1,0x65};
    memset(frame, 0x55, sizeof(frame)); memcpy(frame, header, sizeof(header));
    test_partial_construction(); test_destroy_barrier(); test_concurrent_admission();
    test_idle_and_control();
    for (int ice = 0; ice < 2; ++ice) {
        if ((!ice && !EWRTC_WITH_NATIVE_ICE) || (ice && !EWRTC_WITH_LIBJUICE)) continue;
        test_streams(2, 1, ice); test_streams(4, 2, ice); test_streams(8, 1, ice);
        test_slow_close(ice); test_candidate_dns_failure(ice);
    }
    test_juice_error_wake(); test_juice_media_pressure();
    puts("Shared context acceptance tests passed");
}
