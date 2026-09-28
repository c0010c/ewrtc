#include "private.h"
#include <stdio.h>
#include <string.h>

/* Rewrite hostname candidates before owner-thread protocol calls. Preserve every
 * other SDP field and candidate suffix; .local keeps native peer-reflexive behavior. */
static int normalize_candidate(ewrtc_session *s, char *text, size_t capacity) {
    if (strstr(text, ".local ")) return 0;
    char foundation[32], transport[8], host[128];
    unsigned component, priority, port;
    int begin = 0, end = 0;
    const char *prefix = !strncmp(text, "a=", 2) ? text + 2 : text;
    if (sscanf(prefix, "candidate:%31s %u %7s %u %n%127s%n %u", foundation, &component,
        transport, &priority, &begin, host, &end, &port) != 6 || port > 65535 || !port)
        return 0;
    if (component != 1 || ewrtc_ascii_casecmp(transport, "UDP")) return 0;
    ewrtc_address addr;
    if (!ewrtc_address_parse(host, (uint16_t)port, &addr) || strchr(host, ':')) return 0;
    if (!s->context->pal.network.resolve) return EWRTC_UNSUPPORTED;
    int result = s->context->pal.network.resolve(s->context->pal.network.ctx, host, (uint16_t)port, &addr);
    if (result) return result;
    char ip[32];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(addr.ipv4 >> 24),
        (unsigned)((addr.ipv4 >> 16) & 255), (unsigned)((addr.ipv4 >> 8) & 255),
        (unsigned)(addr.ipv4 & 255));
    begin += (int)(prefix - text); end += (int)(prefix - text);
    size_t tail = strlen(text + end), n = strlen(ip);
    if ((size_t)begin + n + tail + 1 > capacity) return EWRTC_INVALID;
    memmove(text + begin + n, text + end, tail + 1);
    memcpy(text + begin, ip, n);
    return 0;
}
static int normalize_input(ewrtc_session *s, work_item *item) {
    if (item->type == WORK_CREATE_OFFER) return 0;
    if (item->type == WORK_REMOTE_CANDIDATE) {
        int result = normalize_candidate(s, (char *)item->data, item->capacity);
        item->length = strlen((char *)item->data);
        return result;
    }
    char *text = (char *)item->data;
    size_t offset = 0;
    while (text[offset]) {
        char *line = text + offset;
        char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length >= 12 && !memcmp(line, "a=candidate:", 12)) {
            char candidate[EWRTC_MAX_CANDIDATE + 3];
            if (length >= sizeof(candidate)) return EWRTC_INVALID;
            memcpy(candidate, line, length); candidate[length] = 0;
            int result = normalize_candidate(s, candidate, sizeof(candidate));
            /* Embedded unsupported/unresolvable candidates were previously
             * ignored by apply_sdp_candidates. Never pass a hostname to an I/O
             * backend when preparation could not resolve it. */
            if (result) candidate[0] = 0;
            size_t n = strlen(candidate), tail = strlen(line + length);
            if (offset + n + tail + 1 > item->capacity) return EWRTC_INVALID;
            memmove(line + n, line + length, tail + 1);
            memcpy(line, candidate, n);
            length = n;
        }
        offset += length;
        if (text[offset] == '\n') ++offset;
    }
    item->length = strlen(text);
    return 0;
}
void session_cleanup_transport(ewrtc_session *s) {
    /* The owner has unregistered and handed over all protocol objects. */
    if (s->ice) { ewrtc_ice_destroy(s->ice); s->ice = NULL; }
    if (s->dtls) { ewrtc_dtls_destroy(s->dtls); s->dtls = NULL; }
    ewrtc_media_destroy(s->media); s->media = NULL;
    ewrtc_srtp_destroy(s->srtp); s->srtp = NULL;
    for (size_t i = 0; i < s->pending_count; ++i)
        ewrtc_free(&s->context->pal, s->pending_candidates[i]);
    s->pending_count = 0;
    session_discard_queue(s);
}
void *session_prepare_main(void *arg) {
    ewrtc_context *c = arg;
    for (;;) {
        context_lock(c);
        ewrtc_session *s = NULL;
        for (unsigned priority = 0; priority < 2 && !s; ++priority)
            for (size_t i = 0; i < c->max_sessions; ++i) {
                ewrtc_session *candidate = c->sessions[i];
                if (candidate && candidate->prepare_state == PREP_QUEUED &&
                    candidate->cleanup == (priority == 0)) { s = candidate; break; }
            }
        if (!s) {
            if (c->stopping) { context_unlock(c); return NULL; }
            int result = c->pal.threads.condition_wait(c->pal.threads.ctx, c->prepare_cv, c->mu, UINT32_MAX);
            if (result && result != EWRTC_TIMEOUT) {
                for (size_t i = 0; i < c->max_sessions; ++i) if (c->sessions[i]) {
                    if (!c->sessions[i]->critical_error) {
                        c->sessions[i]->critical_error = (ewrtc_result)result;
                        c->sessions[i]->critical_detail = "preparation condition wait failed";
                    }
                    session_wake_locked(c->sessions[i]);
                }
            }
            context_unlock(c); continue;
        }
        s->prepare_state = PREP_RUNNING;
        bool cleanup = s->cleanup, stopped = s->stopping;
        work_item *item = s->prepare_item;
        context_unlock(c);
        int result = 0;
        if (cleanup) session_cleanup_transport(s);
        else if (!stopped) {
            s->prepare_detail = "candidate resolution or normalization failed";
            result = normalize_input(s, item);
            if (!result && !session_stopped(s) &&
                (item->type == WORK_CREATE_OFFER || item->type == WORK_OFFER))
                result = session_prepare_transport(s, item->type == WORK_CREATE_OFFER);
            item->prepared = true;
        }
        context_lock(c);
        s->prepare_result = result; s->prepare_state = PREP_DONE;
        session_wake_locked(s);
        context_unlock(c);
    }
}
