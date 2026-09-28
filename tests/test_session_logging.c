#include "session/private.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned locks, adapter_locks, writes, errors;
    uint64_t now;
    char lines[32][1024];
} harness;
static void lock(void *ctx, ewrtc_mutex m) { (void)m; ++((harness *)ctx)->locks; }
static void unlock(void *ctx, ewrtc_mutex m) { (void)m; --((harness *)ctx)->locks; }
static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p) { (void)ctx; free(p); }
static uint64_t mono(void *ctx) { return ((harness *)ctx)->now; }
static void write_log(void *ctx, int level, const char *text) {
    (void)level; harness *h = ctx;
    assert(!h->locks && !h->adapter_locks && h->writes < 32);
    snprintf(h->lines[h->writes++], sizeof(h->lines[0]), "%s", text);
}
static void error(ewrtc_session *s, ewrtc_result r, const char *detail, void *ctx) {
    (void)s; (void)r; (void)detail; ++((harness *)ctx)->errors;
}
static bool contains(harness *h, const char *text) {
    for (unsigned i = 0; i < h->writes; ++i) if (strstr(h->lines[i], text)) return true;
    return false;
}
int main(void) {
    harness h = {0};
    ewrtc_context c = {.log_id = 3, .queue_limit = 4096};
    c.pal = (ewrtc_pal){.memory = {&h, allocate, NULL, release}, .clock = {&h, mono, NULL},
        .threads = {.ctx = &h, .mutex_lock = lock, .mutex_unlock = unlock}, .log = {&h, write_log}};
    ewrtc_session a = {.context = &c, .token = 7, .queue_limit = 2048,
                       .cb = {.on_error = error}, .user = &h};
    ewrtc_session b = {.context = &c, .token = 8};
    bool info = ewrtc_log_enabled(&c.pal, EWRTC_LOG_INFO);
    bool warn = ewrtc_log_enabled(&c.pal, EWRTC_LOG_WARN);
    session_set_state(&a, EWRTC_CONNECTING);
    session_set_state(&a, EWRTC_CONNECTING); /* No duplicate transition. */
    session_set_state(&b, EWRTC_CONNECTING);
    assert(h.writes == (info ? 2u : 0u));
    if (info) assert(contains(&h, "c3/s7") && contains(&h, "c3/s8"));
    h.writes = 0;
    int result;
    /* Simulate the outer libjuice lock across the delivery allocator. */
    h.adapter_locks = 1;
    for (unsigned i = 0; i < 1000; ++i) {
        assert(!session_allocate(&a, 4096, false, &result));
        assert(result == EWRTC_BACKPRESSURE);
    }
    a.ordinary_items = EWRTC_MAX_WORK_ITEMS;
    assert(!session_allocate(&a, 1, false, &result));
    a.ordinary_items = 0;
    c.stats.queue_bytes = c.queue_limit;
    assert(!session_allocate(&a, 1, false, &result));
    c.stats.queue_bytes = 0;
    assert(!session_allocate(&a, 1, true, &result));
    control_slot slot = {0}; a.control_free = &slot;
    assert(!session_allocate(&a, sizeof(slot.data) + 1, true, &result));
    h.adapter_locks = 0;
    assert(!h.writes);
    session_log_summary(&a, false);
    assert(h.writes == (warn ? 5u : 0u));
    if (warn) {
        assert(contains(&h, "session_byte_limit count=1000"));
        assert(contains(&h, "context_byte_limit count=1"));
        assert(contains(&h, "work_item_limit count=1"));
        assert(contains(&h, "control_slots_exhausted count=1"));
        assert(contains(&h, "control_item_too_large count=1"));
    }
    h.writes = 0;
    for (unsigned i = 0; i < 1000; ++i) session_report_send(&a, EWRTC_BACKPRESSURE);
    assert(!h.writes && h.errors == 1000);
    h.now = 4999; session_log_summary(&a, false); assert(!h.writes);
    h.now = 5000; session_log_summary(&a, false);
    assert(h.writes == (warn ? 1u : 0u));
    if (warn) assert(contains(&h, "send_backpressure count=1000"));
    h.writes = 0;
    session_report_send(&a, EWRTC_INVALID);
    session_log_summary(&a, true); /* Closing flush does not lose pending counts. */
    assert(h.writes == (warn ? 1u : 0u));
    if (warn) assert(contains(&h, "send_invalid count=1"));
    h.writes = 0;
    c.pal.log.write = NULL;
    session_set_state(&a, EWRTC_CLOSED);
    session_log_summary(&a, true);
    assert(!h.writes && !h.locks);
    puts("Session scope, queue causes, callback safety and bounded summaries passed");
}
