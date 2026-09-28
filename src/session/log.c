#include "private.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

const char *session_state_name(ewrtc_state state) {
    static const char *const names[] = {
        "new", "gathering", "connecting", "connected", "disconnected", "failed", "closed"
    };
    return (unsigned)state < sizeof(names) / sizeof(*names) ? names[state] : "unknown";
}

void session_log_write(ewrtc_session *s, int level, const char *module, const char *format, ...) {
    char scope[48];
    snprintf(scope, sizeof(scope), "c%u/s%" PRIu64, s->context->log_id, s->token);
    va_list args;
    va_start(args, format);
    ewrtc_log_vwrite(&s->context->pal, level, module, scope, format, args);
    va_end(args);
}

/* Only the owner worker emits summaries, outside all protocol/adapter locks.
 * Producers (including libjuice's locked delivery path) only increment counters. */
void session_log_summary(ewrtc_session *s, bool force) {
    if (!ewrtc_log_enabled(&s->context->pal, EWRTC_LOG_WARN)) return;
    uint64_t now = ewrtc_now_ms(&s->context->pal);
    if (!force && now < s->log_next_summary) return;
    uint64_t counts[LOG_COUNTER_COUNT];
    session_lock(s);
    memcpy(counts, s->log_counts, sizeof(counts));
    memset(s->log_counts, 0, sizeof(s->log_counts));
    session_unlock(s);
    static const struct { const char *module, *reason; } events[] = {
        {"QUEUE", "session_byte_limit"}, {"QUEUE", "context_byte_limit"},
        {"QUEUE", "work_item_limit"}, {"QUEUE", "control_slots_exhausted"},
        {"QUEUE", "control_item_too_large"}, {"MEDIA", "send_backpressure"},
        {"MEDIA", "send_invalid"}, {"SRTP", "unprotect_failed"},
        {"MEDIA", "receive_failed"}, {"MEDIA", "packet_too_large"}
    };
    bool emitted = false;
    for (unsigned i = 0; i < LOG_COUNTER_COUNT; ++i) if (counts[i]) {
        SESSION_LOG(s, EWRTC_LOG_WARN, events[i].module, "summary reason=%s count=%" PRIu64,
                    events[i].reason, counts[i]);
        emitted = true;
    }
    if (emitted) s->log_next_summary = now + 5000;
}
