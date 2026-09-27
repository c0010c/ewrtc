#include "private.h"
#include <string.h>

_Thread_local unsigned ewrtc_callback_depth;
void context_lock(ewrtc_context *c) { c->pal.threads.mutex_lock(c->pal.threads.ctx, c->mu); }
void context_unlock(ewrtc_context *c) { c->pal.threads.mutex_unlock(c->pal.threads.ctx, c->mu); }
void ewrtc_context_config_init(ewrtc_context_config *cfg) {
    if (cfg) memset(cfg, 0, sizeof(*cfg));
}
static void release_context(ewrtc_context *c) {
    ewrtc_pal p = c->pal;
    if (c->workers) {
        for (size_t i = 0; i < c->worker_count; ++i)
            if (c->workers[i].waiter) p.events.destroy(p.events.ctx, c->workers[i].waiter);
    }
    ewrtc_free(&p, c->sessions);
    ewrtc_free(&p, c->workers);
    if (c->prepare_cv) p.threads.condition_destroy(p.threads.ctx, c->prepare_cv);
    if (c->mu) p.threads.mutex_destroy(p.threads.ctx, c->mu);
    ewrtc_free(&p, c);
}
static int stop_threads(ewrtc_context *c) {
    context_lock(c);
    c->stopping = true;
    c->pal.threads.condition_signal(c->pal.threads.ctx, c->prepare_cv);
    for (size_t i = 0; i < c->worker_count; ++i)
        if (c->workers[i].waiter) c->pal.events.wake(c->pal.events.ctx, c->workers[i].waiter);
    context_unlock(c);
    int result = 0;
    if (c->prepare_thread) {
        int r = c->pal.threads.thread_join(c->pal.threads.ctx, c->prepare_thread);
        if (r) result = r; else c->prepare_thread = NULL;
    }
    for (size_t i = 0; i < c->worker_count; ++i) if (c->workers[i].thread) {
        int r = c->pal.threads.thread_join(c->pal.threads.ctx, c->workers[i].thread);
        if (r) result = r; else c->workers[i].thread = NULL;
    }
    return result;
}
ewrtc_result ewrtc_context_create(const ewrtc_context_config *cfg, ewrtc_context **out) {
    if (!out) return EWRTC_INVALID;
    *out = NULL;
    if (!cfg || ewrtc_pal_validate(&cfg->pal, EWRTC_PAL_MEMORY | EWRTC_PAL_CLOCK |
        EWRTC_PAL_RANDOM | EWRTC_PAL_THREADS | EWRTC_PAL_EVENTS)) return EWRTC_INVALID;
    size_t workers = cfg->worker_count ? cfg->worker_count : 1;
    size_t sessions = cfg->max_sessions ? cfg->max_sessions : 8;
    size_t slots = cfg->control_slots ? cfg->control_slots : 64;
    if (workers > sessions || workers > SIZE_MAX / sizeof(session_worker) ||
        sessions > SIZE_MAX / sizeof(ewrtc_session *) || slots > SIZE_MAX / sizeof(control_slot))
        return EWRTC_INVALID;
    ewrtc_context *c = ewrtc_zalloc(&cfg->pal, sizeof(*c));
    if (!c) return EWRTC_NOMEM;
    c->pal = cfg->pal;
    c->worker_count = workers; c->max_sessions = sessions; c->control_slots = slots;
    c->queue_limit = cfg->queue_limit_bytes ? cfg->queue_limit_bytes : 8 * 1024 * 1024;
    c->stats.workers = workers;
    c->stats.sdk_threads = workers + 1;
    int result = c->pal.threads.mutex_create(c->pal.threads.ctx, &c->mu);
    if (result) goto fail;
    result = c->pal.threads.condition_create(c->pal.threads.ctx, &c->prepare_cv);
    if (result) goto fail;
    c->workers = ewrtc_zalloc(&c->pal, workers * sizeof(*c->workers));
    c->sessions = ewrtc_zalloc(&c->pal, sessions * sizeof(*c->sessions));
    if (!c->workers || !c->sessions) { result = EWRTC_NOMEM; goto fail; }
    for (size_t i = 0; i < workers; ++i) {
        c->workers[i].context = c; c->workers[i].index = i;
        result = c->pal.events.create(c->pal.events.ctx, &c->workers[i].waiter);
        if (result) goto threads_fail;
    }
    result = c->pal.threads.thread_create(c->pal.threads.ctx, session_prepare_main, c, &c->prepare_thread);
    if (result) goto threads_fail;
    for (size_t i = 0; i < workers; ++i) {
        result = c->pal.threads.thread_create(c->pal.threads.ctx, session_worker_main,
                                             &c->workers[i], &c->workers[i].thread);
        if (result) goto threads_fail;
    }
    *out = c; return EWRTC_OK;
threads_fail:
    /* A conforming PAL must join its own successfully created threads. */
    if (stop_threads(c)) return EWRTC_IO;
fail:
    release_context(c); return (ewrtc_result)result;
}
ewrtc_result ewrtc_context_destroy(ewrtc_context *c) {
    if (!c) return EWRTC_INVALID;
    if (ewrtc_callback_depth) return EWRTC_STATE;
    context_lock(c);
    bool active = c->stats.sessions != 0;
    context_unlock(c);
    if (active) return EWRTC_STATE;
    if (stop_threads(c)) return EWRTC_IO;
    release_context(c); return EWRTC_OK;
}
ewrtc_result ewrtc_context_get_stats(ewrtc_context *c, ewrtc_context_stats *out) {
    if (!c || !out) return EWRTC_INVALID;
    context_lock(c); *out = c->stats; context_unlock(c); return EWRTC_OK;
}
uint64_t session_callback_begin(ewrtc_session *s) {
    ++ewrtc_callback_depth;
    return ewrtc_now_ms(&s->context->pal);
}
void session_callback_end(ewrtc_session *s, uint64_t at) {
    uint64_t elapsed = ewrtc_now_ms(&s->context->pal) - at;
    --ewrtc_callback_depth;
    context_lock(s->context);
    if (elapsed > s->context->stats.max_callback_ms) s->context->stats.max_callback_ms = elapsed;
    context_unlock(s->context);
}
