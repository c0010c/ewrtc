#include "session_hub.h"
#include "idr_control.h"
#include "ewrtc.h"
#include "ewrtc/platform/linux.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    uint32_t id;
    struct session_hub *hub;
    ewrtc_session *session;
    atomic_bool connected, wait_keyframe, offering;
    atomic_uint connected_ms, first_keyframe_ms;
} peer;
struct session_hub {
    mux_io *io;
    ewrtc_context *context;
    pthread_mutex_t mu; /* protects frame fanout against removal/destruction */
    peer *peers[8];
    unsigned limit;
    uint64_t frames;
    int idr_fd;
    atomic_uint idr_requests, idr_send_errors;
};
static unsigned now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}
static void request_keyframe(peer *p) {
    atomic_fetch_add(&p->hub->idr_requests, 1);
    if (idr_control_request(p->hub->idr_fd)) atomic_fetch_add(&p->hub->idr_send_errors, 1);
}
static void state_cb(ewrtc_session *s, ewrtc_state state, void *user) {
    (void)s; peer *p = user;
    if (state == EWRTC_CONNECTED && !atomic_load(&p->connected)) {
        atomic_store(&p->connected_ms, now_ms());
        atomic_store(&p->first_keyframe_ms, UINT32_MAX);
    }
    atomic_store(&p->connected, state == EWRTC_CONNECTED);
    if (state != EWRTC_CONNECTED) atomic_store(&p->wait_keyframe, true);
    else request_keyframe(p);
    char text[16]; snprintf(text, sizeof(text), "%d", state);
    mux_io_send(p->hub->io, p->id, "STATE", text);
}
static void sdp_cb(ewrtc_session *s, const char *sdp, void *user) {
    (void)s; peer *p = user;
    mux_io_send(p->hub->io, p->id, atomic_load(&p->offering) ? "OFFER" : "ANSWER", sdp);
}
static void candidate_cb(ewrtc_session *s, const char *candidate, void *user) {
    (void)s; peer *p = user; mux_io_send(p->hub->io, p->id, "CANDIDATE", candidate);
}
static void done_cb(ewrtc_session *s, void *user) {
    (void)s; peer *p = user; mux_io_send(p->hub->io, p->id, "DONE", "");
}
static void keyframe_cb(ewrtc_session *s, void *user) {
    (void)s; peer *p = user; atomic_store(&p->wait_keyframe, true);
    if (atomic_load(&p->connected)) request_keyframe(p);
}
static void error_cb(ewrtc_session *s, ewrtc_result code, const char *detail, void *user) {
    (void)s; (void)code; peer *p = user; mux_io_send(p->hub->io, p->id, "ERROR", detail);
}
session_hub *session_hub_create(mux_io *io, unsigned limit) {
    if (!limit || limit > 8) return NULL;
    session_hub *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->io = io; h->limit = limit;
    atomic_init(&h->idr_requests, 0); atomic_init(&h->idr_send_errors, 0);
    h->idr_fd = -1;
    if (pthread_mutex_init(&h->mu, NULL)) { free(h); return NULL; }
    ewrtc_context_config cfg; ewrtc_context_config_init(&cfg);
    cfg.pal = *ewrtc_pal_linux(); cfg.max_sessions = limit;
    cfg.worker_count = 1; cfg.queue_limit_bytes = 2 * 1024 * 1024;
    if (ewrtc_context_create(&cfg, &h->context)) {
        pthread_mutex_destroy(&h->mu); free(h); return NULL;
    }
    h->idr_fd = idr_control_open();
    return h;
}
static int find_peer(session_hub *h, uint32_t id) {
    for (unsigned i = 0; i < h->limit; ++i) if (h->peers[i] && h->peers[i]->id == id) return (int)i;
    return -1;
}
static void remove_peer(session_hub *h, unsigned i) {
    pthread_mutex_lock(&h->mu);
    peer *p = h->peers[i]; h->peers[i] = NULL;
    pthread_mutex_unlock(&h->mu);
    if (!p) return;
    /* No application handle use after removal; callbacks may still run until destroy returns. */
    ewrtc_session_destroy(p->session);
    mux_io_send(h->io, p->id, "CLOSED", "");
    free(p);
}
static void create_peer(session_hub *h, uint32_t id) {
    if (!id || find_peer(h, id) >= 0) { mux_io_send(h->io, id, "ERROR", "Invalid or duplicate session ID"); return; }
    unsigned i = 0; while (i < h->limit && h->peers[i]) ++i;
    if (i == h->limit) { mux_io_send(h->io, id, "ERROR", "观看会话数已达上限，请关闭一个页面后重试。"); return; }
    peer *p = calloc(1, sizeof(*p));
    if (!p) { mux_io_send(h->io, id, "ERROR", "Unable to allocate session"); return; }
    p->id = id; p->hub = h;
    atomic_init(&p->connected, false); atomic_init(&p->wait_keyframe, true); atomic_init(&p->offering, false);
    atomic_init(&p->connected_ms, 0); atomic_init(&p->first_keyframe_ms, UINT32_MAX);
    ewrtc_session_config cfg; ewrtc_session_config_init(&cfg);
    cfg.video_direction = EWRTC_SENDONLY; cfg.audio_direction = EWRTC_INACTIVE;
    cfg.send_queue_limit_bytes = 128 * 1024; cfg.retransmit_cache_limit_bytes = 128 * 1024;
    ewrtc_callbacks cb = {.on_state = state_cb, .on_local_sdp = sdp_cb,
        .on_local_candidate = candidate_cb, .on_gathering_done = done_cb,
        .on_keyframe_request = keyframe_cb, .on_error = error_cb};
    int result = ewrtc_session_create(h->context, &cfg, &cb, p, &p->session);
    if (result) { free(p); mux_io_send(h->io, id, "ERROR", "Unable to create SDK session"); return; }
    pthread_mutex_lock(&h->mu); h->peers[i] = p; pthread_mutex_unlock(&h->mu);
    mux_io_send(h->io, id, "CREATED", "");
}
static void stats(session_hub *h, uint32_t id) {
    ewrtc_context_stats c; ewrtc_context_get_stats(h->context, &c);
    ewrtc_stats s = {0}; int i = find_peer(h, id);
    if (i >= 0) ewrtc_session_get_stats(h->peers[i]->session, &s);
    pthread_mutex_lock(&h->mu); uint64_t frames = h->frames; pthread_mutex_unlock(&h->mu);
    char text[768];
    snprintf(text, sizeof(text), "{\"pid\":%ld,\"sessions\":%zu,\"workers\":%zu,\"sdk_threads\":%zu,\"camera_sources\":1,\"camera_frames\":%llu,\"sent_video_packets\":%llu,\"queue_bytes\":%zu,\"cache_bytes\":%zu,\"backpressure\":%llu,\"idr_requests\":%u,\"idr_send_errors\":%u,\"first_keyframe_wait_ms\":%u}",
        (long)getpid(), c.sessions, c.workers, c.sdk_threads, (unsigned long long)frames,
        (unsigned long long)s.sent_video_packets, s.send_queue_bytes, s.retransmit_cache_bytes,
        (unsigned long long)s.backpressure_count, atomic_load(&h->idr_requests),
        atomic_load(&h->idr_send_errors), i < 0 ? UINT32_MAX : atomic_load(&h->peers[i]->first_keyframe_ms));
    mux_io_send(h->io, id, "STATS", text);
}
void session_hub_command(session_hub *h, const mux_record *r) {
    if (!strcmp(r->kind, "CREATE")) { create_peer(h, r->id); return; }
    if (!strcmp(r->kind, "STATS")) { stats(h, r->id); return; }
    int i = find_peer(h, r->id);
    if (!strcmp(r->kind, "CLOSE")) { if (i >= 0) remove_peer(h, (unsigned)i); return; }
    if (i < 0) { mux_io_send(h->io, r->id, "ERROR", "Unknown session ID"); return; }
    peer *p = h->peers[i]; ewrtc_result result = EWRTC_INVALID;
    if (!strcmp(r->kind, "OFFER")) result = ewrtc_session_set_remote_offer(p->session, r->data);
    else if (!strcmp(r->kind, "ANSWER")) result = ewrtc_session_set_remote_answer(p->session, r->data);
    else if (!strcmp(r->kind, "CANDIDATE")) result = ewrtc_session_add_remote_candidate(p->session, r->data);
    else if (!strcmp(r->kind, "END")) result = ewrtc_session_end_remote_candidates(p->session);
    else if (!strcmp(r->kind, "CREATE_OFFER")) {
        atomic_store(&p->offering, true); result = ewrtc_session_create_offer(p->session);
    }
    if (result) { char text[80]; snprintf(text, sizeof(text), "SDK rejected %s: %d", r->kind, result); mux_io_send(h->io, r->id, "ERROR", text); }
}
void session_hub_frame(const uint8_t *data, size_t size, uint64_t pts, int key, void *user) {
    session_hub *h = user;
    pthread_mutex_lock(&h->mu);
    ++h->frames;
    for (unsigned i = 0; i < h->limit; ++i) {
        peer *p = h->peers[i];
        if (!p || !atomic_load(&p->connected)) continue;
        if (atomic_load(&p->wait_keyframe) && !key) continue;
        if (key) atomic_store(&p->wait_keyframe, false);
        if (ewrtc_session_send_video(p->session, data, size, pts, key) != EWRTC_OK) {
            atomic_store(&p->wait_keyframe, true);
            request_keyframe(p);
        } else if (key && atomic_load(&p->first_keyframe_ms) == UINT32_MAX) {
            atomic_store(&p->first_keyframe_ms, now_ms() - atomic_load(&p->connected_ms));
        }
    }
    pthread_mutex_unlock(&h->mu);
}
void session_hub_source_error(session_hub *h) {
    pthread_mutex_lock(&h->mu);
    for (unsigned i = 0; i < h->limit; ++i) if (h->peers[i]) {
        atomic_store(&h->peers[i]->wait_keyframe, true);
        mux_io_send(h->io, h->peers[i]->id, "ERROR", "摄像头输入中断，请重新连接。");
    }
    pthread_mutex_unlock(&h->mu);
}
void session_hub_destroy(session_hub *h) {
    if (!h) return;
    for (unsigned i = 0; i < h->limit; ++i) remove_peer(h, i);
    ewrtc_context_destroy(h->context);
    if (h->idr_fd >= 0) close(h->idr_fd);
    pthread_mutex_destroy(&h->mu); free(h);
}
