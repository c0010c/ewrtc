#include "private.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

void session_set_state(ewrtc_session *s, ewrtc_state state) {
    session_lock(s);
    ewrtc_state previous = s->stats.state;
    bool changed = previous != state;
    s->stats.state = state;
    session_unlock(s);
    if (changed)
        SESSION_LOG(s, state == EWRTC_DISCONNECTED ? EWRTC_LOG_WARN : EWRTC_LOG_INFO,
                    "SESSION", "state %s -> %s age=%" PRIu64 "ms",
                    session_state_name(previous), session_state_name(state),
                    ewrtc_now_ms(&s->context->pal) - s->created_ms);
    if (changed && s->cb.on_state)
        SESSION_CALLBACK(s, s->cb.on_state(s, state, s->user));
}

void session_error(ewrtc_session *s, ewrtc_result code, const char *detail) {
    SESSION_LOG(s, EWRTC_LOG_ERROR, "SESSION", "%s code=%d", detail, code);
    if (s->cb.on_error)
        SESSION_CALLBACK(s, s->cb.on_error(s, code, detail, s->user));
}

static int ewrtc_send_wire(ewrtc_session *s, const uint8_t *data, size_t size) {
    return s->ice ? ewrtc_ice_send(s->ice, data, size) : EWRTC_STATE;
}

static int send_media_packet(void *user, const uint8_t *data, size_t size, bool rtcp, int kind) {
    (void)kind;
    ewrtc_session *s = user;
    uint8_t packet[2048];
    if (size > sizeof(packet) - 32)
        return EWRTC_INVALID;
    memcpy(packet, data, size);
    int result = ewrtc_srtp_protect(s->srtp, rtcp, packet, sizeof(packet), &size);
    if (result)
        return result;
    if (size > EWRTC_MTU)
        return EWRTC_INVALID;
    result = ewrtc_send_wire(s, packet, size);
    if (result)
        return result;
    session_lock(s);
    s->stats.sent_bytes += size;
    session_unlock(s);
    return 0;
}

static void media_audio(void *user, const uint8_t *data, size_t size, uint32_t ts, uint16_t seq) {
    ewrtc_session *s = user;
    if (s->cb.on_audio)
        SESSION_CALLBACK(s, s->cb.on_audio(s, data, size, ts, seq, s->user));
}

static void media_keyframe(void *user) {
    ewrtc_session *s = user;
    if (s->cb.on_keyframe_request)
        SESSION_CALLBACK(s, s->cb.on_keyframe_request(s, s->user));
}

static void handle_ice_state(ewrtc_session *, ewrtc_state);

static void ice_state(void *user, ewrtc_state state) {
    ewrtc_session *s = user;
    /* libjuice drain owns no protocol/queue lock here: the event's existing
     * reservation covers the complete delivery. Native callbacks defer until
     * its receive/timer invocation unwinds. */
    if (s->cfg.ice_backend == EWRTC_ICE_LIBJUICE) handle_ice_state(s, state);
    else session_internal_event(s, WORK_ICE_STATE, NULL, 0, state);
}

static void ice_candidate(void *user, const char *candidate) {
    ewrtc_session *s = user;
    if (!candidate || strlen(candidate) > EWRTC_MAX_CANDIDATE) return;
    if (s->cfg.ice_backend == EWRTC_ICE_LIBJUICE) {
        if (s->cb.on_local_candidate)
            SESSION_CALLBACK(s, s->cb.on_local_candidate(s, candidate, s->user));
    } else session_internal_event(s, WORK_ICE_CANDIDATE, candidate, strlen(candidate), EWRTC_NEW);
}

static void ice_done(void *user) {
    ewrtc_session *s = user;
    if (s->cfg.ice_backend == EWRTC_ICE_LIBJUICE) {
        if (s->cb.on_gathering_done) SESSION_CALLBACK(s, s->cb.on_gathering_done(s, s->user));
    } else session_internal_event(s, WORK_ICE_DONE, NULL, 0, EWRTC_NEW);
}

static void ice_recv(void *user, const uint8_t *data, size_t size) {
    ewrtc_session *s = user;
    if (size <= 2048)
        session_handle_wire(s, data, size);
}

static int dtls_send(void *user, const uint8_t *data, size_t size) {
    return ewrtc_send_wire(user, data, size);
}

static void media_video(void *user, const uint8_t *data, size_t size, uint32_t ts, int keyframe) {
    ewrtc_session *s = user;
    if (s->cb.on_video) SESSION_CALLBACK(s, s->cb.on_video(s, data, size, ts, keyframe, s->user));
}
static void negotiation_error(ewrtc_session *s, int result, const char *detail) {
    session_error(s, (ewrtc_result)result, detail);
    session_fail_at(s, result, detail);
    session_set_state(s, EWRTC_FAILED);
}
int session_prepare_transport(ewrtc_session *s, bool controlling) {
    uint32_t ids[3];
    s->prepare_detail = "transport random generation failed";
    if (ewrtc_random_bytes(&s->context->pal, ids, sizeof(ids))) return EWRTC_SECURITY;
    s->video_ssrc = ids[0]; s->rtx_ssrc = ids[1]; s->audio_ssrc = ids[2];
    ewrtc_dtls_config dc = {.pal = s->context->pal, .backend = s->cfg.dtls_backend,
                            .send = dtls_send, .user = s};
    s->prepare_detail = "DTLS creation failed";
    int result = ewrtc_dtls_create(&dc, &s->dtls);
    if (result) return result;
    ewrtc_ice_events events = {ice_state, ice_candidate, ice_done, ice_recv, s};
    ewrtc_ice_config ic = {.pal = s->context->pal, .backend = s->cfg.ice_backend,
        .crypto_backend = s->cfg.crypto_backend, .stun_host = s->cfg.stun_host,
        .stun_port = s->cfg.stun_port, .turn_host = s->cfg.turn_host,
        .turn_port = s->cfg.turn_port, .turn_username = s->cfg.turn_username,
        .turn_password = s->cfg.turn_password, .relay_only = s->cfg.relay_only,
        .controlling = controlling,
        .delivery = {.user = s, .allocate = session_delivery_allocate, .release = session_release,
                     .notify = session_notify, .stopped = session_stopped, .yield = session_yield}};
    s->prepare_detail = "ICE creation failed (backend, socket or server resolution)";
    return ewrtc_ice_create(&ic, &events, &s->ice);
}
static ewrtc_direction negotiate_direction(ewrtc_direction local, ewrtc_direction remote) {
    bool send = ewrtc_direction_sends(local) && ewrtc_direction_receives(remote);
    bool recv = ewrtc_direction_receives(local) && ewrtc_direction_sends(remote);
    return send ? (recv ? EWRTC_SENDRECV : EWRTC_SENDONLY) : (recv ? EWRTC_RECVONLY : EWRTC_INACTIVE);
}
static int set_remote(ewrtc_session *s, const ewrtc_sdp_offer *remote, ewrtc_sdp_offer *local) {
    s->dtls_client = local->setup == EWRTC_SETUP_ACTIVE;
    int result = ewrtc_dtls_set_peer(s->dtls, remote->fingerprint, s->dtls_client);
    if (result) return result;
    result = ewrtc_ice_remote_credentials(s->ice, remote->ice_ufrag, remote->ice_pwd);
    if (result) return result;
    ewrtc_media_config mc = {.pal = s->context->pal,
        .video_pt = remote->video_pt, .rtx_pt = remote->rtx_pt, .audio_pt = remote->audio_pt,
        .video_ssrc = s->video_ssrc, .rtx_ssrc = s->rtx_ssrc, .audio_ssrc = s->audio_ssrc,
        .cache_limit_bytes = s->cfg.retransmit_cache_limit_bytes,
        .cache_max_age_ms = s->cfg.retransmit_cache_max_age_ms,
        .send = send_media_packet,
        .audio = ewrtc_direction_receives(local->audio_direction) ? media_audio : NULL,
        .video = ewrtc_direction_receives(local->video_direction) ? media_video : NULL,
        .keyframe = ewrtc_direction_sends(local->video_direction) ? media_keyframe : NULL,
        .user = s, .remote_video_ssrc = remote->video_ssrc, .remote_rtx_ssrc = remote->rtx_ssrc,
        .nack = remote->nack, .pli = remote->pli};
    result = ewrtc_media_create(&mc, &s->media);
    if (result) return result;
    session_lock(s);
    s->send_video = ewrtc_direction_sends(local->video_direction);
    s->send_audio = ewrtc_direction_sends(local->audio_direction);
    session_unlock(s);
    s->remote_set = true;
    for (size_t i = 0; i < s->pending_count; ++i) {
        result = ewrtc_ice_add_candidate(s->ice, s->pending_candidates[i]);
        if (result) return result;
    }
    for (size_t i = 0; i < s->pending_count; ++i) ewrtc_free(&s->context->pal, s->pending_candidates[i]);
    s->pending_count = 0;
    return s->pending_end ? ewrtc_ice_end_candidates(s->ice) : 0;
}
static int emit_description(ewrtc_session *s, const ewrtc_sdp_offer *description, bool offer) {
    char ufrag[256], pwd[256], sdp[4096];
    int result = ewrtc_ice_credentials(s->ice, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
    if (result) return result;
    ewrtc_sdp_local local = {ufrag, pwd, ewrtc_dtls_fingerprint(s->dtls)};
    result = ewrtc_sdp_make_description(description, &local, s->video_ssrc, s->rtx_ssrc,
                                        s->audio_ssrc, sdp, sizeof(sdp));
    if (result) return result;
    SESSION_LOG(s, EWRTC_LOG_INFO, "SDP", "%s ready video_pt=%d audio_pt=%d",
                offer ? "offer" : "answer", description->video_pt, description->audio_pt);
    if (offer) {
        session_lock(s);
        s->local_offer_ready = true;
        session_unlock(s);
    }
    if (s->cb.on_local_sdp) SESSION_CALLBACK(s, s->cb.on_local_sdp(s, sdp, s->user));
    session_set_state(s, EWRTC_GATHERING);
    return ewrtc_ice_start(s->ice);
}
/* Accept candidates embedded in a gathered SDP as well as trickled candidates. */
static int apply_sdp_candidates(ewrtc_session *s, const char *text) {
    bool end = false;
    for (const char *line = text; *line;) {
        const char *next = strchr(line, '\n');
        size_t size = next ? (size_t)(next - line) : strlen(line);
        if (size && line[size - 1] == '\r') --size;
        if (size >= 12 && !memcmp(line, "a=candidate:", 12)) {
            if (size - 2 > EWRTC_MAX_CANDIDATE) return EWRTC_INVALID;
            char candidate[EWRTC_MAX_CANDIDATE + 1];
            memcpy(candidate, line + 2, size - 2); candidate[size - 2] = 0;
            /* This SDK supports IPv4/UDP; ignore other candidate transports. */
            char foundation[32], transport[8]; unsigned component;
            if (sscanf(candidate, "candidate:%31s %u %7s", foundation, &component, transport) == 3 &&
                component == 1 && !ewrtc_ascii_casecmp(transport, "UDP")) {
                int result = ewrtc_ice_add_candidate(s->ice, candidate);
                if (result && result != EWRTC_INVALID) return result;
            }
        } else if (size == 19 && !memcmp(line, "a=end-of-candidates", 19)) end = true;
        if (!next) break;
        line = next + 1;
    }
    return end ? ewrtc_ice_end_candidates(s->ice) : 0;
}
static void handle_offer(ewrtc_session *s, const char *text) {
    int result = ewrtc_sdp_parse_offer(&s->context->pal, text, &s->offer);
    if (result) { negotiation_error(s, result, "Unsupported SDP offer"); return; }
    ewrtc_sdp_offer local = s->offer;
    local.video_direction = negotiate_direction(s->cfg.video_direction, s->offer.video_direction);
    local.audio_direction = negotiate_direction(s->cfg.audio_direction, s->offer.audio_direction);
    local.setup = s->offer.setup == EWRTC_SETUP_PASSIVE ? EWRTC_SETUP_ACTIVE : EWRTC_SETUP_PASSIVE;
    result = set_remote(s, &s->offer, &local);
    if (result) { negotiation_error(s, result, "Remote transport configuration failed"); return; }
    result = apply_sdp_candidates(s, text);
    if (result) { negotiation_error(s, result, "Embedded ICE candidate application failed"); return; }
    result = emit_description(s, &local, false);
    if (result) negotiation_error(s, result, "Local answer or ICE gathering failed");
}
static void handle_create_offer(ewrtc_session *s) {
    ewrtc_sdp_offer *o = &s->local_offer;
    *o = (ewrtc_sdp_offer){.video_pt = 102, .rtx_pt = 103, .audio_pt = 111,
        .video_mid = "0", .audio_mid = "1", .h264_profile = "42e01f",
        .video = true, .audio = true, .bundle = true, .rtcp_mux = true, .trickle = true,
        .video_direction = s->cfg.video_direction, .audio_direction = s->cfg.audio_direction,
        .setup = EWRTC_SETUP_ACTPASS, .nack = true, .pli = true};
    int result = 0;
    if (!result) result = emit_description(s, o, true);
    if (result) negotiation_error(s, result, "Local offer initialization failed");
}
static void handle_answer(ewrtc_session *s, const char *text) {
    int result = ewrtc_sdp_parse_answer(&s->context->pal, text, &s->offer);
    if (!result) result = ewrtc_sdp_validate_answer(&s->local_offer, &s->offer);
    if (result) { negotiation_error(s, result, "SDP answer does not match local offer"); return; }
    ewrtc_sdp_offer local = s->local_offer;
    local.video_direction = negotiate_direction(local.video_direction, s->offer.video_direction);
    local.audio_direction = negotiate_direction(local.audio_direction, s->offer.audio_direction);
    local.setup = s->offer.setup == EWRTC_SETUP_PASSIVE ? EWRTC_SETUP_ACTIVE : EWRTC_SETUP_PASSIVE;
    result = set_remote(s, &s->offer, &local);
    if (!result) result = apply_sdp_candidates(s, text);
    if (result) negotiation_error(s, result, "Remote answer initialization failed");
}

static void handle_ice_state(ewrtc_session *s, ewrtc_state state) {
    if (s->stats.state == EWRTC_FAILED)
        return;
    if (state == EWRTC_CONNECTED) {
        session_update_path(s);
        if (s->cfg.relay_only && s->cfg.ice_backend == EWRTC_ICE_LIBJUICE &&
            !strstr(s->stats.local_candidate, " typ relay") &&
            !strstr(s->stats.remote_candidate, " typ relay")) {
            negotiation_error(s, EWRTC_IO, "TURN path was not selected");
            return;
        }
        if (!s->dtls_started && s->dtls) {
            s->dtls_started = true;
            s->dtls_started_ms = ewrtc_now_ms(&s->context->pal);
            SESSION_LOG(s, EWRTC_LOG_INFO, "DTLS", "handshake started role=%s",
                        s->dtls_client ? "client" : "server");
            session_set_state(s, EWRTC_CONNECTING);
            int result = ewrtc_dtls_start(s->dtls);
            if (result && result != EWRTC_AGAIN && result != EWRTC_BACKPRESSURE) {
                negotiation_error(s, result, "DTLS handshake start failed");
            }
        }
    } else if (state == EWRTC_FAILED || state == EWRTC_DISCONNECTED) {
        session_set_state(s, state);
        if (state == EWRTC_FAILED) {
            SESSION_LOG(s, EWRTC_LOG_ERROR, "ICE", "connectivity failed code=%d", EWRTC_IO);
            session_fail_at(s, EWRTC_IO, "ICE connectivity failed");
        }
    } else if (state == EWRTC_GATHERING && s->stats.state == EWRTC_NEW) {
        session_set_state(s, state);
    }
}

void session_handle_wire(ewrtc_session *s, const uint8_t *data, size_t len) {
    if (!len)
        return;
    if (data[0] >= 20 && data[0] <= 63 && s->dtls_started && s->dtls &&
        s->stats.state != EWRTC_FAILED) {
        int result = ewrtc_dtls_receive(s->dtls, data, len);
        if (result && result != EWRTC_AGAIN && result != EWRTC_BACKPRESSURE) {
            negotiation_error(s, result, "DTLS handshake failed");
        } else session_finish_dtls(s);
    } else if (data[0] >= 128 && data[0] <= 191 && s->stats.state == EWRTC_CONNECTED) {
        uint8_t plain[2048];
        bool rtcp = len >= 2 && data[1] >= 192 && data[1] <= 223;
        int result = EWRTC_INVALID;
        enum session_log_counter failure = LOG_WIRE_SIZE;
        if (len <= sizeof(plain)) {
            memcpy(plain, data, len);
            failure = LOG_SRTP_RECEIVE;
            result = ewrtc_srtp_unprotect(s->srtp, rtcp, plain, &len);
            if (!result) {
                failure = LOG_MEDIA_RECEIVE;
                result = ewrtc_media_receive(s->media, plain, len, rtcp);
            }
        }
        if (result) {
            session_lock(s);
            s->stats.dropped_packets++;
            ++s->log_counts[failure];
            session_unlock(s);
            if (result == EWRTC_NOMEM) session_fail_at(s, result, "media receive allocation failed");
        } else {
            session_lock(s);
            s->stats.received_bytes += len;
            session_unlock(s);
        }
        session_sync_media_stats(s);
    }
}

void session_finish_dtls(ewrtc_session *s) {
    if (s->stats.state == EWRTC_FAILED || !s->remote_set || !s->dtls || s->srtp ||
        !ewrtc_dtls_connected(s->dtls)) return;
    uint8_t keys[60];
    int result = ewrtc_dtls_export_keys(s->dtls, keys);
    if (!result) result = ewrtc_srtp_create(&s->context->pal, keys, !s->dtls_client, &s->srtp);
    if (result) negotiation_error(s, result, "SRTP initialization failed");
    else {
        SESSION_LOG(s, EWRTC_LOG_INFO, "DTLS", "handshake completed elapsed=%" PRIu64 "ms srtp=ready",
                    ewrtc_now_ms(&s->context->pal) - s->dtls_started_ms);
        session_set_state(s, EWRTC_CONNECTED);
    }
}

void session_report_send(ewrtc_session *s, int result) {
    if (!result)
        return;
    session_lock(s);
    ++s->stats.dropped_packets;
    bool temporary = result == EWRTC_AGAIN || result == EWRTC_BACKPRESSURE;
    if (temporary) ++s->stats.backpressure_count;
    if (temporary || result == EWRTC_INVALID)
        ++s->log_counts[temporary ? LOG_SEND_PRESSURE : LOG_SEND_INVALID];
    session_unlock(s);
    if (temporary || result == EWRTC_INVALID) {
        /* Preserve business callbacks while bounding diagnostic output. */
        if (s->cb.on_error)
            SESSION_CALLBACK(s, s->cb.on_error(s, (ewrtc_result)result, "media send failed", s->user));
    } else session_error(s, (ewrtc_result)result, "media send failed");
    /* Partially emitted frames are not replayed; temporary pressure is nonfatal. */
    if (result != EWRTC_AGAIN && result != EWRTC_BACKPRESSURE && result != EWRTC_INVALID) {
        session_fail_at(s, result, "media send failed");
        session_set_state(s, EWRTC_FAILED);
    }
}

void session_process(ewrtc_session *s, work_item *item) {
    if (s->stats.state == EWRTC_FAILED)
        return;
    switch (item->type) {
    case WORK_CREATE_OFFER:
        handle_create_offer(s);
        break;
    case WORK_ANSWER:
        handle_answer(s, (const char *)item->data);
        break;
    case WORK_OFFER:
        handle_offer(s, (const char *)item->data);
        break;
    case WORK_REMOTE_CANDIDATE:
        if (!s->remote_set) {
            if (s->pending_count == 32) {
                negotiation_error(s, EWRTC_BACKPRESSURE, "Too many early ICE candidates");
                break;
            }
            char *copy = ewrtc_strdup(&s->context->pal, (const char *)item->data);
            if (!copy) { negotiation_error(s, EWRTC_NOMEM, "ICE candidate allocation failed"); break; }
            s->pending_candidates[s->pending_count++] = copy;
        } else if (ewrtc_ice_add_candidate(s->ice, (const char *)item->data))
            session_error(s, EWRTC_INVALID, "remote ICE candidate rejected");
        break;
    case WORK_END_CANDIDATES:
        s->pending_end = true;
        if (s->remote_set) ewrtc_ice_end_candidates(s->ice);
        break;
    case WORK_VIDEO:
    case WORK_AUDIO:
        if (s->stats.state == EWRTC_CONNECTED) {
            int result = item->type == WORK_VIDEO
                ? ewrtc_media_send_video(s->media, item->data, item->length, item->pts_us, item->keyframe)
                : ewrtc_media_send_audio(s->media, item->data, item->length, item->pts_us);
            session_report_send(s, result);
        }
        break;
    case WORK_ICE_STATE:
        handle_ice_state(s, item->state);
        break;
    case WORK_ICE_CANDIDATE:
        if (s->cb.on_local_candidate)
            SESSION_CALLBACK(s, s->cb.on_local_candidate(s, (const char *)item->data, s->user));
        break;
    case WORK_ICE_DONE:
        if (s->cb.on_gathering_done)
            SESSION_CALLBACK(s, s->cb.on_gathering_done(s, s->user));
        break;
    }
}

