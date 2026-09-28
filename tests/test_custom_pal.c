/* Application-owned C11 services, no Linux PAL or platform calls in the SDK. */
#include "ewrtc.h"
#include "offer.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

static _Thread_local unsigned lock_depth;
typedef struct { atomic_uint logs; atomic_size_t live; atomic_uint opened, closed, answers, callbacks, threads; } harness;
static void *allocate(void *ctx, size_t n) {
    void *p = malloc(n); if (p) ++((harness *)ctx)->live; return p;
}
static void *resize(void *ctx, void *p, size_t n) {
    bool fresh = !p; void *q = realloc(p, n); if (q && fresh) ++((harness *)ctx)->live; return q;
}
static void release(void *ctx, void *p) { if (p) { --((harness *)ctx)->live; free(p); } }
static uint64_t mono(void *ctx) {
    (void)ctx; struct timespec t; timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
}
static uint64_t utc(void *ctx) { return mono(ctx) * 1000; }
static int random_bytes(void *ctx, void *p, size_t n) { (void)ctx; memset(p, 7, n); return 0; }
typedef struct { thrd_t thread; void *(*fn)(void *); void *arg; } test_thread;
static int entry(void *arg) { test_thread *t = arg; t->fn(t->arg); return 0; }
static int thread_create(void *ctx, void *(*fn)(void *), void *arg, ewrtc_thread *out) {
    test_thread *t = allocate(ctx, sizeof(*t)); if (!t) return EWRTC_NOMEM;
    t->fn = fn; t->arg = arg;
    if (thrd_create(&t->thread, entry, t) != thrd_success) { release(ctx, t); return EWRTC_IO; }
    ++((harness *)ctx)->threads; *out = t; return 0;
}
static int thread_join(void *ctx, ewrtc_thread handle) {
    test_thread *t = handle; assert(thrd_join(t->thread, NULL) == thrd_success);
    --((harness *)ctx)->threads; release(ctx, t); return 0;
}
static int current(void *ctx, ewrtc_thread handle) {
    (void)ctx; return thrd_equal(((test_thread *)handle)->thread, thrd_current());
}
static int mutex_create(void *ctx, ewrtc_mutex *out) {
    mtx_t *m = allocate(ctx, sizeof(*m)); if (!m) return EWRTC_NOMEM;
    assert(mtx_init(m, mtx_plain) == thrd_success); *out = m; return 0;
}
static void mutex_destroy(void *ctx, ewrtc_mutex m) { mtx_destroy(m); release(ctx, m); }
static void lock(void *ctx, ewrtc_mutex m) { (void)ctx; assert(mtx_lock(m) == thrd_success); ++lock_depth; }
static void unlock(void *ctx, ewrtc_mutex m) { (void)ctx; --lock_depth; assert(mtx_unlock(m) == thrd_success); }
static int condition_create(void *ctx, ewrtc_condition *out) {
    cnd_t *c = allocate(ctx, sizeof(*c)); if (!c) return EWRTC_NOMEM;
    assert(cnd_init(c) == thrd_success); *out = c; return 0;
}
static void condition_destroy(void *ctx, ewrtc_condition c) { cnd_destroy(c); release(ctx, c); }
static void signal_condition(void *ctx, ewrtc_condition c) { (void)ctx; cnd_signal(c); }
static int wait_condition(void *ctx, ewrtc_condition c, ewrtc_mutex m, uint32_t ms) {
    (void)ctx;
    if (ms == UINT32_MAX) return cnd_wait(c, m) == thrd_success ? 0 : EWRTC_IO;
    struct timespec t; timespec_get(&t, TIME_UTC);
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) { ++t.tv_sec; t.tv_nsec -= 1000000000; }
    int r = cnd_timedwait(c, m, &t);
    return r == thrd_timedout ? EWRTC_TIMEOUT : r == thrd_success ? 0 : EWRTC_IO;
}
typedef struct { ewrtc_mutex mu; ewrtc_condition cv; bool wake; } test_waiter;
static int event_create(void *ctx, ewrtc_waiter *out) {
    test_waiter *w = allocate(ctx, sizeof(*w)); assert(w);
    assert(!mutex_create(ctx, &w->mu) && !condition_create(ctx, &w->cv));
    w->wake = false; *out = w; return 0;
}
static void event_destroy(void *ctx, ewrtc_waiter handle) {
    test_waiter *w = handle; condition_destroy(ctx, w->cv); mutex_destroy(ctx, w->mu); release(ctx, w);
}
static int event_add(void *ctx, ewrtc_waiter w, ewrtc_socket s, uint64_t token) {
    (void)ctx; (void)w; (void)s; assert(token); return 0;
}
static int event_remove(void *ctx, ewrtc_waiter w, ewrtc_socket s) {
    (void)ctx; (void)w; (void)s; return 0;
}
static int event_wake(void *ctx, ewrtc_waiter handle) {
    test_waiter *w = handle; lock(ctx, w->mu); w->wake = true;
    signal_condition(ctx, w->cv); unlock(ctx, w->mu); return 0;
}
static int event_wait(void *ctx, ewrtc_waiter handle, uint64_t *tokens, size_t cap, size_t *n, uint32_t ms) {
    (void)tokens; (void)cap; *n = 0;
    test_waiter *w = handle; lock(ctx, w->mu);
    if (!w->wake) wait_condition(ctx, w->cv, w->mu, ms);
    w->wake = false; unlock(ctx, w->mu); return 0;
}
static int udp_open(void *ctx, const ewrtc_address *bind, ewrtc_socket *out) {
    (void)bind; ++((harness *)ctx)->opened; *out = allocate(ctx, 1); return *out ? 0 : EWRTC_NOMEM;
}
static void udp_close(void *ctx, ewrtc_socket socket) { ++((harness *)ctx)->closed; release(ctx, socket); }
static int udp_local(void *ctx, ewrtc_socket socket, ewrtc_address *out) {
    (void)ctx; (void)socket; *out = (ewrtc_address){0x7f000001, 12345}; return 0;
}
static int udp_send(void *ctx, ewrtc_socket socket, const ewrtc_address *to, const void *p, size_t n) {
    (void)ctx; (void)socket; (void)to; (void)p; (void)n; return 0;
}
static int udp_receive(void *ctx, ewrtc_socket socket, ewrtc_address *from, void *p, size_t cap, size_t *n) {
    (void)ctx; (void)socket; (void)from; (void)p; (void)cap; *n = 0; return EWRTC_AGAIN;
}
static int resolve(void *ctx, const char *host, uint16_t port, ewrtc_address *out) {
    (void)ctx; return ewrtc_address_parse(host, port, out);
}
static int interfaces(void *ctx, ewrtc_address *out, size_t cap, size_t *count) {
    (void)ctx; assert(cap); *out = (ewrtc_address){0x7f000001, 0}; *count = 1; return 0;
}
static void answer(ewrtc_session *s, const char *text, void *ctx) {
    assert(strstr(text, "a=setup:passive")); ++((harness *)ctx)->answers; assert(!ewrtc_session_close(s));
}
static void state(ewrtc_session *s, ewrtc_state value, void *ctx) {
    (void)value; ++((harness *)ctx)->callbacks;
    ewrtc_stats stats; assert(!ewrtc_session_get_stats(s, &stats));
    assert(ewrtc_session_destroy(s) == EWRTC_STATE);
}
static void log_write(void *ctx, int level, const char *text) {
    (void)level;
    assert(!lock_depth && !strchr(text, '\n'));
    ++((harness *)ctx)->logs;
}
static void run(bool logging) {
    harness h = {0}; ewrtc_context_config cc; ewrtc_context_config_init(&cc);
    assert(!cc.pal.memory.allocate);
    cc.pal = (ewrtc_pal){.memory = {&h, allocate, resize, release}, .clock = {&h, mono, utc},
        .random = {&h, random_bytes},
        .threads = {&h, thread_create, thread_join, current, mutex_create, mutex_destroy, lock, unlock,
                    condition_create, condition_destroy, signal_condition, wait_condition},
        .network = {&h, udp_open, udp_close, udp_local, udp_send, udp_receive, resolve, interfaces},
        .events = {&h, event_create, event_destroy, event_add, event_remove, event_wait, event_wake}};
    if (logging) cc.pal.log = (ewrtc_logger){&h, log_write};
    ewrtc_context *context; assert(!ewrtc_context_create(&cc, &context));
    ewrtc_session_config cfg; ewrtc_session_config_init(&cfg);
    ewrtc_callbacks cb = {.on_local_sdp = answer, .on_state = state}; ewrtc_session *a, *b;
    assert(!ewrtc_session_create(context, &cfg, &cb, &h, &a));
    assert(!ewrtc_session_create(context, &cfg, &cb, &h, &b));
    assert(!ewrtc_session_set_remote_offer(a, test_offer));
    assert(!ewrtc_session_set_remote_offer(b, test_offer));
    for (unsigned i = 0; i < 5000 && h.answers < 2; ++i) {
        struct timespec t = {0, 1000000}; thrd_sleep(&t, NULL);
    }
    assert(h.answers == 2 && h.threads == 2);
    assert(!ewrtc_session_destroy(a) && !ewrtc_session_destroy(b));
    assert(h.opened == h.closed);
    unsigned callbacks = h.callbacks;
    assert(!ewrtc_context_destroy(context) && !h.live && !h.threads && h.callbacks == callbacks);
    if (!logging) assert(!h.logs);
    puts("Shared context with application C11 PAL passed");
}

int main(void) { run(false); run(true); }
