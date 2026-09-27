#include "private.h"
#include <stdio.h>
#include <string.h>

void ewrtc_session_config_init(ewrtc_session_config *c) {
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->ice_backend = EWRTC_WITH_NATIVE_ICE ? EWRTC_ICE_NATIVE : EWRTC_ICE_LIBJUICE;
    c->dtls_backend = EWRTC_WITH_OPENSSL ? EWRTC_DTLS_OPENSSL : EWRTC_DTLS_MBEDTLS;
    c->crypto_backend = EWRTC_WITH_OPENSSL ? EWRTC_CRYPTO_OPENSSL : EWRTC_CRYPTO_MBEDTLS;
}

static void release_session(ewrtc_session *s) {
    ewrtc_pal *p = &s->context->pal;
    ewrtc_free(p, s->stun_host); ewrtc_free(p, s->turn_host);
    ewrtc_free(p, s->turn_username); ewrtc_free(p, s->turn_password);
    ewrtc_free(p, s->control_pool);
    if (s->cv) p->threads.condition_destroy(p->threads.ctx, s->cv);
    ewrtc_free(p, s);
}

ewrtc_result ewrtc_session_create(ewrtc_context *c, const ewrtc_session_config *config,
                                  const ewrtc_callbacks *callbacks, void *user, ewrtc_session **out) {
    if (!out) return EWRTC_INVALID;
    *out = NULL;
    if (!c || !config || !callbacks || (unsigned)config->video_direction > EWRTC_INACTIVE ||
        (unsigned)config->audio_direction > EWRTC_INACTIVE ||
        (unsigned)config->ice_backend > EWRTC_ICE_LIBJUICE ||
        (unsigned)config->dtls_backend > EWRTC_DTLS_MBEDTLS ||
        (config->relay_only && (!config->turn_host || !*config->turn_host)) ||
        (config->ice_backend == EWRTC_ICE_NATIVE && ewrtc_pal_validate(&c->pal, EWRTC_PAL_NETWORK)))
        return EWRTC_INVALID;
    if ((config->ice_backend == EWRTC_ICE_LIBJUICE && !EWRTC_WITH_LIBJUICE) ||
        (config->ice_backend == EWRTC_ICE_NATIVE && !EWRTC_WITH_NATIVE_ICE) ||
        (config->dtls_backend == EWRTC_DTLS_OPENSSL && !EWRTC_WITH_OPENSSL) ||
        (config->dtls_backend == EWRTC_DTLS_MBEDTLS && !EWRTC_WITH_MBEDTLS) ||
        (config->ice_backend == EWRTC_ICE_NATIVE && !ewrtc_crypto_available(config->crypto_backend)))
        return EWRTC_UNSUPPORTED;
    size_t reserved = c->control_slots * sizeof(control_slot);
    context_lock(c);
    if (c->stopping) { context_unlock(c); return EWRTC_STATE; }
    if (c->stats.sessions == c->max_sessions ||
        reserved > c->queue_limit - c->stats.control_reserved_bytes - c->stats.queue_bytes) {
        context_unlock(c); return EWRTC_BACKPRESSURE;
    }
    /* Reserve admission before constructing outside the shared I/O lock. */
    ++c->stats.sessions; c->stats.control_reserved_bytes += reserved;
    context_unlock(c);
    ewrtc_session *s = ewrtc_zalloc(&c->pal, sizeof(*s));
    if (!s) {
        context_lock(c); --c->stats.sessions; c->stats.control_reserved_bytes -= reserved;
        context_unlock(c); return EWRTC_NOMEM;
    }
    s->context = c; s->cfg = *config; s->cb = *callbacks; s->user = user;
    s->deadline = UINT64_MAX;
    s->queue_limit = config->send_queue_limit_bytes ? config->send_queue_limit_bytes : 1024 * 1024;
    int result = c->pal.threads.condition_create(c->pal.threads.ctx, &s->cv);
    if (result) goto fail;
    s->control_pool = ewrtc_zalloc(&c->pal, reserved);
    if (!s->control_pool) { result = EWRTC_NOMEM; goto fail; }
    for (size_t i = 0; i < c->control_slots; ++i) {
        s->control_pool[i].next = s->control_free; s->control_free = &s->control_pool[i];
    }
    s->stats.ice_backend = config->ice_backend; s->stats.dtls_backend = config->dtls_backend;
    s->stun_host = ewrtc_strdup(&c->pal, config->stun_host);
    s->turn_host = ewrtc_strdup(&c->pal, config->turn_host);
    s->turn_username = ewrtc_strdup(&c->pal, config->turn_username);
    s->turn_password = ewrtc_strdup(&c->pal, config->turn_password);
    s->cfg.stun_host = s->stun_host; s->cfg.turn_host = s->turn_host;
    s->cfg.turn_username = s->turn_username; s->cfg.turn_password = s->turn_password;
    if ((config->stun_host && !s->stun_host) || (config->turn_host && !s->turn_host) ||
        (config->turn_username && !s->turn_username) || (config->turn_password && !s->turn_password)) {
        result = EWRTC_NOMEM; goto fail;
    }
    context_lock(c);
    size_t best = SIZE_MAX, worker = 0, slot = 0;
    for (size_t i = 0; i < c->worker_count; ++i) {
        size_t count = 0;
        for (size_t j = 0; j < c->max_sessions; ++j)
            if (c->sessions[j] && c->sessions[j]->worker == &c->workers[i]) ++count;
        if (count < best) { best = count; worker = i; }
    }
    while (c->sessions[slot]) ++slot;
    s->worker = &c->workers[worker];
    s->token = ++c->next_token;
    if (!s->token) s->token = ++c->next_token;
    c->sessions[slot] = s;
    context_unlock(c); *out = s; return EWRTC_OK;
fail:
    release_session(s);
    context_lock(c); --c->stats.sessions; c->stats.control_reserved_bytes -= reserved;
    context_unlock(c); return (ewrtc_result)result;
}

ewrtc_result ewrtc_session_create_offer(ewrtc_session *s) {
    if (!s) return EWRTC_INVALID;
    session_lock(s);
    bool allowed = !s->has_offer && !s->stopping;
    if (allowed) { s->has_offer = true; s->local_offerer = true; }
    session_unlock(s);
    if (!allowed) return EWRTC_STATE;
    ewrtc_result result = session_enqueue(s, WORK_CREATE_OFFER, NULL, 0, 0, EWRTC_NEW, 0);
    if (result) {
        session_lock(s);
        s->has_offer = s->local_offerer = false;
        session_unlock(s);
    }
    return result;
}

ewrtc_result ewrtc_session_set_remote_answer(ewrtc_session *s, const char *sdp) {
    if (!s || !sdp || !*sdp || strlen(sdp) > EWRTC_MAX_SDP) return EWRTC_INVALID;
    session_lock(s);
    bool allowed = s->local_offerer && s->local_offer_ready && !s->has_answer && !s->stopping &&
                   s->stats.state != EWRTC_FAILED;
    if (allowed) s->has_answer = true;
    session_unlock(s);
    if (!allowed) return EWRTC_STATE;
    ewrtc_result result = session_enqueue(s, WORK_ANSWER, sdp, strlen(sdp), 0, EWRTC_NEW, 0);
    if (result) {
        session_lock(s);
        s->has_answer = false;
        session_unlock(s);
    }
    return result;
}

ewrtc_result ewrtc_session_set_remote_offer(ewrtc_session *s, const char *sdp) {
    if (!s || !sdp || !*sdp || strlen(sdp) > EWRTC_MAX_SDP)
        return EWRTC_INVALID;
    session_lock(s);
    bool allowed = !s->has_offer && !s->stopping;
    if (allowed)
        s->has_offer = true;
    session_unlock(s);
    if (!allowed)
        return EWRTC_STATE;
    ewrtc_result result = session_enqueue(s, WORK_OFFER, sdp, strlen(sdp), 0, EWRTC_NEW, 0);
    if (result != EWRTC_OK) {
        session_lock(s);
        s->has_offer = false;
        session_unlock(s);
    }
    return result;
}

ewrtc_result ewrtc_session_add_remote_candidate(ewrtc_session *s, const char *candidate) {
    if (!s || !candidate || !*candidate || strlen(candidate) > EWRTC_MAX_CANDIDATE)
        return EWRTC_INVALID;
    session_lock(s);
    bool allowed = s->has_offer && !s->stopping;
    session_unlock(s);
    if (!allowed)
        return EWRTC_STATE;
    return session_enqueue(s, WORK_REMOTE_CANDIDATE, candidate, strlen(candidate), 0, EWRTC_NEW, 0);
}

ewrtc_result ewrtc_session_end_remote_candidates(ewrtc_session *s) {
    if (!s)
        return EWRTC_INVALID;
    session_lock(s);
    bool allowed = s->has_offer && !s->stopping;
    session_unlock(s);
    if (!allowed)
        return EWRTC_STATE;
    return session_enqueue(s, WORK_END_CANDIDATES, NULL, 0, 0, EWRTC_NEW, 0);
}

static ewrtc_result send_media(ewrtc_session *s, enum work_type type, const uint8_t *data,
                               size_t length, uint64_t pts_us, int keyframe) {
    if (!s || !data || !length || length > EWRTC_MAX_FRAME)
        return EWRTC_INVALID;
    session_lock(s);
    bool connected = s->stats.state == EWRTC_CONNECTED && !s->stopping &&
                     (type == WORK_VIDEO ? s->send_video : s->send_audio);
    session_unlock(s);
    if (!connected)
        return EWRTC_STATE;
    return session_enqueue(s, type, data, length, pts_us, EWRTC_NEW, keyframe);
}

ewrtc_result ewrtc_session_send_video(ewrtc_session *s, const uint8_t *data, size_t size,
                                      uint64_t pts_us, int keyframe) {
    if (size > EWRTC_MAX_FRAME || ewrtc_media_validate_video(data, size, keyframe))
        return EWRTC_INVALID;
    return send_media(s, WORK_VIDEO, data, size, pts_us, keyframe);
}

ewrtc_result ewrtc_session_send_audio(ewrtc_session *s, const uint8_t *data, size_t size,
                                      uint64_t pts_us) {
    if (size > EWRTC_MAX_OPUS_PACKET_BYTES)
        return EWRTC_INVALID;
    return send_media(s, WORK_AUDIO, data, size, pts_us, 0);
}

ewrtc_result ewrtc_session_close(ewrtc_session *s) {
    if (!s)
        return EWRTC_INVALID;
    session_lock(s);
    s->stopping = true;
    session_wake_locked(s);
    session_unlock(s);
    return EWRTC_OK;
}

ewrtc_result ewrtc_session_destroy(ewrtc_session *s) {
    if (!s) return EWRTC_INVALID;
    if (ewrtc_callback_depth) return EWRTC_STATE;
    ewrtc_context *c = s->context;
    session_lock(s);
    s->stopping = true;
    session_wake_locked(s);
    while (!s->closed || s->busy) {
        int result = c->pal.threads.condition_wait(c->pal.threads.ctx, s->cv, c->mu, UINT32_MAX);
        if (result && result != EWRTC_TIMEOUT) { session_unlock(s); return EWRTC_IO; }
    }
    for (size_t i = 0; i < c->max_sessions; ++i) if (c->sessions[i] == s) c->sessions[i] = NULL;
    /* Keep the context admission lock until all session PAL accesses have ended. */
    --c->stats.sessions;
    c->stats.control_reserved_bytes -= c->control_slots * sizeof(control_slot);
    release_session(s);
    context_unlock(c);
    return EWRTC_OK;
}
