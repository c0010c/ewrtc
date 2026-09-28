#include "private.h"
#include <string.h>

void session_lock(ewrtc_session *s) { context_lock(s->context); }
void session_unlock(ewrtc_session *s) { context_unlock(s->context); }
void session_signal_worker(ewrtc_session *s) {
    s->context->pal.events.wake(s->context->pal.events.ctx, s->worker->waiter);
}
void session_wake_locked(ewrtc_session *s) {
    if (!s->notified) s->ready_at = ewrtc_now_ms(&s->context->pal);
    s->notified = true;
    session_signal_worker(s);
}
bool session_stopped(void *user) {
    ewrtc_session *s = user;
    session_lock(s); bool stopped = s->stopping; session_unlock(s); return stopped;
}
void session_notify(void *user) {
    ewrtc_session *s = user;
    session_lock(s);
    if (!s->closing) session_wake_locked(s);
    session_unlock(s);
}
void session_fail(ewrtc_session *s, int result) {
    session_fail_at(s, result, "session delivery or protocol failure");
}
void session_fail_at(ewrtc_session *s, int result, const char *detail) {
    session_lock(s);
    if (!s->critical_error) {
        s->critical_error = (ewrtc_result)result;
        s->critical_detail = detail; /* static diagnostic string */
    }
    s->stopping = true;
    session_wake_locked(s);
    session_unlock(s);
}
void *session_allocate(void *user, size_t size, bool control, int *error) {
    ewrtc_session *s = user;
    ewrtc_context *c = s->context;
    *error = 0;
    if (size > SIZE_MAX - sizeof(queue_header)) { *error = EWRTC_INVALID; return NULL; }
    size_t bytes = sizeof(queue_header) + size;
    session_lock(s);
    if (s->stopping) { *error = EWRTC_STATE; session_unlock(s); return NULL; }
    if (control) {
        control_slot *slot = s->control_free;
        if (!slot || size > sizeof(slot->data)) {
            ++s->log_counts[!slot ? LOG_QUEUE_CONTROL : LOG_QUEUE_CONTROL_SIZE];
            *error = EWRTC_BACKPRESSURE;
            session_unlock(s); return NULL;
        }
        s->control_free = slot->next;
        slot->header.slot = slot;
        session_unlock(s);
        return slot->data;
    }
    if (s->ordinary_items >= EWRTC_MAX_WORK_ITEMS || bytes > s->queue_limit - s->ordinary_bytes ||
        bytes > c->queue_limit - c->stats.control_reserved_bytes - c->stats.queue_bytes) {
        ++s->log_counts[s->ordinary_items >= EWRTC_MAX_WORK_ITEMS ? LOG_QUEUE_ITEMS :
            bytes > s->queue_limit - s->ordinary_bytes ? LOG_QUEUE_SESSION : LOG_QUEUE_CONTEXT];
        ++c->stats.backpressure_count; ++s->stats.backpressure_count;
        *error = EWRTC_BACKPRESSURE; session_unlock(s); return NULL;
    }
    ++s->ordinary_items; s->ordinary_bytes += bytes;
    c->stats.queue_bytes += bytes;
    if (c->stats.queue_bytes > c->stats.peak_queue_bytes) c->stats.peak_queue_bytes = c->stats.queue_bytes;
    s->stats.send_queue_bytes = s->ordinary_bytes;
    session_unlock(s);
    queue_header *h = ewrtc_alloc(&c->pal, bytes);
    if (h) { h->bytes = bytes; h->slot = NULL; return h + 1; }
    session_lock(s);
    --s->ordinary_items; s->ordinary_bytes -= bytes; c->stats.queue_bytes -= bytes;
    s->stats.send_queue_bytes = s->ordinary_bytes;
    session_unlock(s);
    *error = EWRTC_NOMEM; return NULL;
}
void session_release(void *user, void *data) {
    if (!data) return;
    ewrtc_session *s = user;
    queue_header *h = (queue_header *)data - 1;
    session_lock(s);
    if (h->slot) {
        control_slot *slot = h->slot;
        slot->next = s->control_free; s->control_free = slot;
        session_unlock(s); return;
    }
    size_t bytes = h->bytes;
    session_unlock(s);
    ewrtc_free(&s->context->pal, h);
    session_lock(s);
    --s->ordinary_items; s->ordinary_bytes -= bytes;
    s->context->stats.queue_bytes -= bytes;
    s->stats.send_queue_bytes = s->ordinary_bytes;
    session_unlock(s);
}
ewrtc_result session_enqueue(ewrtc_session *s, enum work_type type, const void *data, size_t len,
                            uint64_t pts, ewrtc_state state, int keyframe) {
    if (!s || (len && !data) || len > EWRTC_MAX_FRAME) return EWRTC_INVALID;
    size_t capacity = len + 1;
    if (type == WORK_OFFER || type == WORK_ANSWER) capacity = EWRTC_MAX_SDP + 1;
    if (type == WORK_REMOTE_CANDIDATE) capacity = EWRTC_MAX_CANDIDATE + 1;
    bool control = type >= WORK_ICE_STATE;
    int error;
    work_item *item = session_allocate(s, sizeof(*item) + capacity, control, &error);
    if (!item) {
        return (ewrtc_result)error;
    }
    *item = (work_item){.type = type, .length = len, .pts_us = pts, .state = state,
                        .keyframe = keyframe, .capacity = capacity};
    if (len) memcpy(item->data, data, len);
    item->data[len] = 0;
    session_lock(s);
    if (s->stopping) { session_unlock(s); session_release(s, item); return EWRTC_STATE; }
    work_item **head = control ? &s->control_head : &s->head;
    work_item **tail = control ? &s->control_tail : &s->tail;
    if (*tail) (*tail)->next = item; else *head = item;
    *tail = item;
    session_wake_locked(s);
    session_unlock(s);
    return EWRTC_OK;
}
void session_internal_event(ewrtc_session *s, enum work_type type, const void *data,
                           size_t size, ewrtc_state state) {
    int result = session_enqueue(s, type, data, size, 0, state, 0);
    if (result && result != EWRTC_STATE) session_fail_at(s, result, "internal event enqueue failed");
}
void session_discard_queue(ewrtc_session *s) {
    work_item *p = s->head;
    s->head = s->tail = NULL;
    while (p) { work_item *next = p->next; session_release(s, p); p = next; }
    p = s->control_head;
    s->control_head = s->control_tail = NULL;
    while (p) { work_item *next = p->next; session_release(s, p); p = next; }
}

void *session_delivery_allocate(void *user, size_t size, bool control, int *error) {
    ewrtc_session *s = user;
    void *p = session_allocate(user, size, control, error);
    if (!p && !control && *error == EWRTC_BACKPRESSURE) {
        session_lock(s);
        ++s->context->stats.congestion_drops; ++s->stats.dropped_packets;
        session_unlock(s);
    }
    return p;
}

bool session_yield(void *user) {
    ewrtc_session *s = user;
    session_lock(s); bool pending = s->control_head != NULL; session_unlock(s);
    return pending;
}
