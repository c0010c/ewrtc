#include "private.h"

static void queue_prepare(ewrtc_session *s, bool cleanup, work_item *item) {
    session_lock(s);
    s->cleanup = cleanup; s->prepare_item = item; s->prepare_state = PREP_QUEUED;
    s->context->pal.threads.condition_signal(s->context->pal.threads.ctx, s->context->prepare_cv);
    session_unlock(s);
}
static work_item *pop(ewrtc_session *s, bool control) {
    session_lock(s);
    work_item **head = control ? &s->control_head : &s->head;
    work_item **tail = control ? &s->control_tail : &s->tail;
    work_item *item = *head;
    if (item) { *head = item->next; if (!*head) *tail = NULL; item->next = NULL; }
    session_unlock(s);
    return item;
}
static void close_step(ewrtc_session *s) {
    ewrtc_context *c = s->context;
    session_lock(s);
    bool report = !s->closing && s->critical_error && s->stats.state != EWRTC_FAILED;
    int critical = s->critical_error;
    s->closing = true;
    enum prepare_state state = s->prepare_state;
    bool cleanup = s->cleanup;
    s->deadline = UINT64_MAX;
    session_unlock(s);
    if (report) {
        session_error(s, (ewrtc_result)critical, "session delivery or protocol failure");
        session_set_state(s, EWRTC_FAILED);
    }
    if (s->registered_socket) {
        int result = c->pal.events.remove(c->pal.events.ctx, s->worker->waiter, s->registered_socket);
        if (result) session_error(s, (ewrtc_result)result, "socket unregistration failed");
        s->registered_socket = NULL;
    }
    if (state == PREP_QUEUED || state == PREP_RUNNING) return;
    if (state == PREP_DONE && cleanup) {
        session_set_state(s, EWRTC_CLOSED);
        session_lock(s); s->closed = true; s->prepare_state = PREP_NONE; session_unlock(s);
        return;
    }
    if (state == PREP_DONE) {
        session_release(s, s->prepare_item);
        s->prepare_item = NULL;
    }
    queue_prepare(s, true, NULL);
}
static void drive_error(ewrtc_session *s, int result) {
    if (!result || result == EWRTC_AGAIN) return;
    session_fail(s, result);
}
static uint64_t next_deadline(ewrtc_session *s) {
    uint64_t next = ewrtc_ice_next_deadline(s->ice);
    uint64_t at = ewrtc_dtls_next_deadline(s->dtls);
    if (at < next) next = at;
    at = ewrtc_media_next_deadline(s->media);
    return at < next ? at : next;
}
static void drive_session(ewrtc_session *s) {
    session_lock(s);
    int critical = s->critical_error;
    if (critical) s->stopping = true;
    bool stopping = s->stopping;
    enum prepare_state state = s->prepare_state;
    session_unlock(s);
    if (stopping) { close_step(s); return; }
    if ((state == PREP_QUEUED || state == PREP_RUNNING) && !s->remote_set) return;
    unsigned commands = 32, packets = 64;
    if (state == PREP_DONE) {
        work_item *item = s->prepare_item;
        int result = s->prepare_result;
        session_lock(s); s->prepare_state = PREP_NONE; s->prepare_item = NULL; session_unlock(s);
        if (result && item->type == WORK_REMOTE_CANDIDATE && result != EWRTC_NOMEM)
            session_error(s, EWRTC_INVALID, "remote ICE candidate resolution failed");
        else if (result) session_fail(s, result);
        else { session_process(s, item); --commands; }
        session_release(s, item);
    }
    while ((state == PREP_NONE || state == PREP_DONE) && commands && !session_stopped(s)) {
        work_item *item = pop(s, true);
        if (!item) item = pop(s, false);
        if (!item) break;
        if (!item->prepared && (item->type == WORK_CREATE_OFFER || item->type == WORK_OFFER ||
            item->type == WORK_ANSWER || item->type == WORK_REMOTE_CANDIDATE)) {
            if (!s->remote_set) s->deadline = UINT64_MAX;
            queue_prepare(s, false, item);
            break;
        }
        --commands;
        session_process(s, item);
        session_release(s, item);
    }
    if (session_stopped(s)) { close_step(s); return; }
    session_lock(s); bool preparing = s->prepare_state != PREP_NONE; session_unlock(s);
    /* Candidate DNS only owns its work item, so established transport can run. */
    if (preparing && !s->remote_set) { s->deadline = UINT64_MAX; return; }
    if (s->stats.state == EWRTC_FAILED) { session_fail(s, EWRTC_STATE); return; }
    if (s->ice && !s->registered_socket) {
        ewrtc_socket socket = ewrtc_ice_socket(s->ice);
        if (socket) {
            int result = s->context->pal.events.add(s->context->pal.events.ctx,
                s->worker->waiter, socket, s->token);
            if (result) { session_fail(s, result); return; }
            s->registered_socket = socket;
        }
    }
    bool more = false;
    if (s->ice) {
        drive_error(s, ewrtc_ice_drain(s->ice, &packets, &commands, &more));
        if (!session_stopped(s)) drive_error(s, ewrtc_ice_timers(s->ice));
    }
    /* Native synchronous state events are deferred until the component returns. */
    while (commands && !session_stopped(s)) {
        work_item *item = pop(s, true);
        if (!item) break;
        --commands; session_process(s, item); session_release(s, item);
    }
    if (!session_stopped(s)) {
        if (s->dtls_started && s->dtls && !ewrtc_dtls_connected(s->dtls)) {
            int result = ewrtc_dtls_tick(s->dtls);
            if (result != EWRTC_BACKPRESSURE) drive_error(s, result);
        }
        session_finish_dtls(s);
        if (s->stats.state == EWRTC_CONNECTED) session_report_send(s, ewrtc_media_tick(s->media));
        session_sync_media_stats(s);
    }
    session_lock(s);
    s->deadline = next_deadline(s);
    if (more) session_wake_locked(s);
    session_unlock(s);
}
static bool runnable(ewrtc_session *s, uint64_t now) {
    if (s->closed) return false;
    if (s->notified || s->prepare_state == PREP_DONE) return true;
    if (s->closing) return false;
    if (s->prepare_state != PREP_NONE) return s->remote_set && s->deadline <= now;
    return s->head || s->control_head || s->deadline <= now;
}
void *session_worker_main(void *arg) {
    session_worker *w = arg;
    ewrtc_context *c = w->context;
    for (;;) {
        context_lock(c);
        if (c->stopping) { context_unlock(c); return NULL; }
        uint64_t now = ewrtc_now_ms(&c->pal), next = UINT64_MAX;
        ewrtc_session *selected = NULL;
        for (size_t n = 0; n < c->max_sessions; ++n) {
            size_t i = (w->cursor + n) % c->max_sessions;
            ewrtc_session *s = c->sessions[i];
            if (!s || s->worker != w || s->closed) continue;
            if (runnable(s, now)) { selected = s; w->cursor = (i + 1) % c->max_sessions; break; }
            if (!s->closing && s->deadline < next) next = s->deadline;
        }
        if (selected) {
            ewrtc_session *s = selected;
            s->busy = true;
            if (s->notified && now - s->ready_at > c->stats.max_schedule_wait_ms)
                c->stats.max_schedule_wait_ms = now - s->ready_at;
            if (s->deadline <= now && now - s->deadline > c->stats.max_timer_delay_ms)
                c->stats.max_timer_delay_ms = now - s->deadline;
            s->notified = false;
            context_unlock(c);
            drive_session(s);
            uint64_t elapsed = ewrtc_now_ms(&c->pal) - now;
            context_lock(c);
            if (elapsed > c->stats.max_turn_ms) c->stats.max_turn_ms = elapsed;
            s->busy = false;
            if (s->closed) c->pal.threads.condition_signal(c->pal.threads.ctx, s->cv);
            context_unlock(c);
            continue;
        }
        context_unlock(c);
        uint32_t timeout = next == UINT64_MAX ? UINT32_MAX : next <= now ? 0 :
            next - now >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)(next - now);
        uint64_t tokens[64]; size_t count = 0;
        int result = c->pal.events.wait(c->pal.events.ctx, w->waiter, tokens, 64, &count, timeout);
        context_lock(c);
        for (size_t i = 0; i < c->max_sessions; ++i) {
            ewrtc_session *s = c->sessions[i];
            if (!s || s->worker != w || s->closed) continue;
            if (result) { if (!s->critical_error) s->critical_error = (ewrtc_result)result; session_wake_locked(s); }
            for (size_t j = 0; j < count; ++j) if (tokens[j] == s->token && !s->closing) {
                if (!s->notified) s->ready_at = ewrtc_now_ms(&c->pal);
                s->notified = true;
            }
        }
        context_unlock(c);
    }
}
