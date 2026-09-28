#ifndef EWRTC_SESSION_PRIVATE_H
#define EWRTC_SESSION_PRIVATE_H
#include "ewrtc.h"
#include "ice/ice.h"
#include "dtls/dtls.h"
#include "sdp/sdp.h"
#include "media/media.h"
#include "srtp/srtp.h"
#define EWRTC_MAX_WORK_ITEMS 4096u
enum work_type {
    WORK_OFFER,
    WORK_CREATE_OFFER,
    WORK_ANSWER,
    WORK_REMOTE_CANDIDATE,
    WORK_END_CANDIDATES,
    WORK_VIDEO,
    WORK_AUDIO,
    WORK_ICE_STATE,
    WORK_ICE_CANDIDATE,
    WORK_ICE_DONE
};
typedef struct work_item {
    struct work_item *next;
    enum work_type type;
    size_t length;
    uint64_t pts_us;
    int keyframe;
    ewrtc_state state;
    bool prepared;
    size_t capacity;
    uint8_t data[];
} work_item;

typedef struct session_worker session_worker;
typedef struct control_slot control_slot;
typedef struct {
    max_align_t alignment;
    size_t bytes;
    control_slot *slot;
} queue_header;
struct control_slot {
    control_slot *next;
    queue_header header;
    unsigned char data[2048 + 128];
};
enum prepare_state { PREP_NONE, PREP_QUEUED, PREP_RUNNING, PREP_DONE };
struct session_worker {
    ewrtc_context *context;
    ewrtc_thread thread;
    ewrtc_waiter waiter;
    size_t index, cursor;
};
enum session_log_counter {
    LOG_QUEUE_SESSION, LOG_QUEUE_CONTEXT, LOG_QUEUE_ITEMS, LOG_QUEUE_CONTROL,
    LOG_QUEUE_CONTROL_SIZE, LOG_SEND_PRESSURE, LOG_SEND_INVALID,
    LOG_SRTP_RECEIVE, LOG_MEDIA_RECEIVE, LOG_WIRE_SIZE, LOG_COUNTER_COUNT
};
struct ewrtc_context {
    ewrtc_pal pal;
    ewrtc_mutex mu;
    ewrtc_condition prepare_cv;
    ewrtc_thread prepare_thread;
    session_worker *workers;
    ewrtc_session **sessions;
    size_t worker_count, max_sessions, queue_limit, control_slots;
    bool stopping;
    uint64_t next_token;
    unsigned log_id;
    ewrtc_context_stats stats;
};
struct ewrtc_session {
    ewrtc_context *context;
    session_worker *worker;
    ewrtc_condition cv;
    uint64_t token, deadline, ready_at;
    ewrtc_socket registered_socket;
    bool stopping, closed, busy, notified, closing, cleanup, has_offer, dtls_started;
    enum prepare_state prepare_state;
    work_item *prepare_item;
    int prepare_result;
    control_slot *control_pool, *control_free;
    size_t ordinary_bytes, ordinary_items;
    work_item *control_head, *control_tail;
    bool local_offerer, local_offer_ready, has_answer, remote_set, dtls_client;
    bool send_video, send_audio;
    char *pending_candidates[32];
    size_t pending_count;
    bool pending_end;
    ewrtc_sdp_offer local_offer;
    work_item *head, *tail;
    size_t queue_limit;
    ewrtc_session_config cfg;
    char *stun_host, *turn_host, *turn_username, *turn_password;
    ewrtc_callbacks cb;
    void *user;
    ewrtc_stats stats;
    ewrtc_result critical_error;
    const char *critical_detail, *prepare_detail;
    uint64_t created_ms, dtls_started_ms, log_next_summary;
    uint64_t log_counts[LOG_COUNTER_COUNT]; /* protected by context mutex */
    ewrtc_sdp_offer offer;
    ewrtc_ice *ice;
    ewrtc_dtls *dtls;
    ewrtc_media *media;
    ewrtc_srtp *srtp;
    uint32_t video_ssrc, rtx_ssrc, audio_ssrc;
};

void session_lock(ewrtc_session *s);
void session_unlock(ewrtc_session *s);
void session_signal_worker(ewrtc_session *s);
ewrtc_result session_enqueue(ewrtc_session *s, enum work_type type, const void *data, size_t len,
                            uint64_t pts, ewrtc_state state, int keyframe);
void session_internal_event(ewrtc_session *s, enum work_type type, const void *data,
                           size_t size, ewrtc_state state);
void *session_worker_main(void *arg);
void *session_prepare_main(void *arg);
void context_lock(ewrtc_context *);
void context_unlock(ewrtc_context *);
void session_wake_locked(ewrtc_session *);
bool session_stopped(void *);
bool session_yield(void *);
void session_notify(void *);
void *session_allocate(void *, size_t, bool, int *);
void *session_delivery_allocate(void *, size_t, bool, int *);
void session_release(void *, void *);
void session_discard_queue(ewrtc_session *);
int session_prepare_transport(ewrtc_session *, bool);
void session_cleanup_transport(ewrtc_session *);
void session_handle_wire(ewrtc_session *, const uint8_t *, size_t);
void session_fail(ewrtc_session *, int);
void session_fail_at(ewrtc_session *, int, const char *);
void session_log_write(ewrtc_session *, int, const char *, const char *, ...) EWRTC_PRINTF(4, 5);
void session_log_summary(ewrtc_session *, bool);
const char *session_state_name(ewrtc_state);
#define SESSION_LOG(s, level, module, ...) do { \
    if (ewrtc_log_enabled(&(s)->context->pal, (level))) \
        session_log_write((s), (level), (module), __VA_ARGS__); \
} while (0)
extern _Thread_local unsigned ewrtc_callback_depth;
uint64_t session_callback_begin(ewrtc_session *);
void session_callback_end(ewrtc_session *, uint64_t);
#define SESSION_CALLBACK(s, expression) do { \
    uint64_t callback_at = session_callback_begin(s); \
    expression; \
    session_callback_end(s, callback_at); \
} while (0)
void session_sync_media_stats(ewrtc_session *s);
void session_update_path(ewrtc_session *s);
void session_set_state(ewrtc_session *s, ewrtc_state state);
void session_error(ewrtc_session *s, ewrtc_result code, const char *detail);
void session_process(ewrtc_session *s, work_item *item);
void session_finish_dtls(ewrtc_session *s);
void session_report_send(ewrtc_session *s, int result);
#endif
