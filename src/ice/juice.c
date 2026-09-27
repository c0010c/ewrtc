#include "private.h"
#include <juice/juice.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct juice_event {
    struct juice_event *next;
    int type;
    ewrtc_state state;
    size_t size;
    uint8_t data[];
} juice_event;
typedef struct {
    ice_adapter base;
    juice_agent_t *agent;
    bool relay_only;
    int delivery_error;
    ewrtc_mutex mutex;
    juice_event *head, *tail;
    size_t queued_count, queued_bytes;
} juice_adapter;

/* libjuice's logging API is process-global, without user context. Disable it
 * once; adapter diagnostics use the instance PAL. Never route across instances. */
static atomic_flag log_guard = ATOMIC_FLAG_INIT;
static bool log_initialized;
static void init_juice_logging(void) {
    while (atomic_flag_test_and_set_explicit(&log_guard, memory_order_acquire)) {
    }
    if (!log_initialized) {
        juice_set_log_level(JUICE_LOG_LEVEL_NONE);
        log_initialized = true;
    }
    atomic_flag_clear_explicit(&log_guard, memory_order_release);
}
static void lock(juice_adapter *j) {
    j->base.pal.threads.mutex_lock(j->base.pal.threads.ctx, j->mutex);
}
static void unlock(juice_adapter *j) {
    j->base.pal.threads.mutex_unlock(j->base.pal.threads.ctx, j->mutex);
}
static void release_event(juice_adapter *j, juice_event *e) {
    if (!e) return;
    if (j->base.delivery.release) j->base.delivery.release(j->base.delivery.user, e);
    else ewrtc_free(&j->base.pal, e);
}
static void notify(juice_adapter *j) {
    if (j->base.delivery.notify) j->base.delivery.notify(j->base.delivery.user);
}
static void queue_event(juice_adapter *j, int type, ewrtc_state state, const void *data,
                        size_t size) {
    ewrtc_ice_delivery *d = &j->base.delivery;
    if (d->stopped && d->stopped(d->user)) return;
    const uint8_t *packet = data;
    bool media = type == 3 && size >= 2 && (packet[0] & 0xc0) == 0x80 &&
                 !(packet[1] >= 192 && packet[1] <= 223);
    lock(j);
    int error = 0;
    juice_event *e = NULL;
    if (size > 2048) error = EWRTC_INVALID;
    /* Integrated queues use the caller's per-session and global admission. */
    else if (!d->allocate && (j->queued_count >= 256 || j->queued_bytes + size > 512 * 1024))
        error = EWRTC_BACKPRESSURE;
    else if (d->allocate) e = d->allocate(d->user, sizeof(*e) + size + 1, !media, &error);
    else {
        e = ewrtc_alloc(&j->base.pal, sizeof(*e) + size + 1);
        if (!e) error = EWRTC_NOMEM;
    }
    if (!e) {
        bool first = !j->delivery_error && !(media && error == EWRTC_BACKPRESSURE);
        if (first) j->delivery_error = error ? error : EWRTC_NOMEM;
        unlock(j);
        if (first) notify(j);
        return;
    }
    *e = (juice_event){.type = type, .state = state, .size = size};
    if (size) memcpy(e->data, data, size);
    e->data[size] = 0;
    if (j->tail) j->tail->next = e;
    else j->head = e;
    j->tail = e;
    ++j->queued_count;
    j->queued_bytes += size;
    unlock(j);
    notify(j);
}
static void state_cb(juice_agent_t *agent, juice_state_t state, void *user) {
    (void)agent;
    juice_adapter *j = user;
    ewrtc_state s = EWRTC_CONNECTING;
    if (state == JUICE_STATE_GATHERING)
        s = EWRTC_GATHERING;
    else if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED)
        s = EWRTC_CONNECTED;
    else if (state == JUICE_STATE_DISCONNECTED)
        s = EWRTC_DISCONNECTED;
    else if (state == JUICE_STATE_FAILED)
        s = EWRTC_FAILED;
    queue_event(j, 0, s, NULL, 0);
}
static void candidate_cb(juice_agent_t *agent, const char *candidate, void *user) {
    (void)agent;
    juice_adapter *j = user;
    if (!j->relay_only || strstr(candidate, " typ relay"))
        queue_event(j, 1, 0, candidate, strlen(candidate));
}
static void done_cb(juice_agent_t *agent, void *user) {
    (void)agent;
    queue_event(user, 2, 0, NULL, 0);
}
static void recv_cb(juice_agent_t *agent, const char *data, size_t size, void *user) {
    (void)agent;
    queue_event(user, 3, 0, data, size);
}

static int get_credential(const char *description, const char *name, char *out, size_t capacity) {
    const char *p = strstr(description, name);
    if (!p)
        return EWRTC_INVALID;
    p += strlen(name);
    const char *end = strpbrk(p, "\r\n");
    if (!end || (size_t)(end - p) >= capacity)
        return EWRTC_INVALID;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = 0;
    return 0;
}

static int from_juice(int result) {
    switch (result) {
    case JUICE_ERR_SUCCESS: case JUICE_ERR_IGNORED: return EWRTC_OK;
    case JUICE_ERR_INVALID: case JUICE_ERR_TOO_LARGE: return EWRTC_INVALID;
    case JUICE_ERR_NOT_AVAIL: return EWRTC_STATE;
    case JUICE_ERR_AGAIN: return EWRTC_AGAIN;
    default: return EWRTC_IO;
    }
}
static int juice_start(ice_adapter *base) {
    juice_adapter *j = (juice_adapter *)base;
    return from_juice(juice_gather_candidates(j->agent));
}
static int juice_drain(ice_adapter *base, unsigned *packets, unsigned *events, bool *more) {
    juice_adapter *j = (juice_adapter *)base;
    *more = false;
    for (;;) {
        if (base->delivery.stopped && base->delivery.stopped(base->delivery.user)) return 0;
        if (base->delivery.yield && base->delivery.yield(base->delivery.user)) {
            *more = true; return 0;
        }
        lock(j);
        if (j->delivery_error) { int error = j->delivery_error; unlock(j); return error; }
        juice_event *e = j->head;
        if (!e) { unlock(j); return 0; }
        unsigned *budget = e->type == 3 ? packets : events;
        if (!*budget) { *more = true; unlock(j); return 0; }
        --*budget;
        j->head = e->next;
        if (!j->head) j->tail = NULL;
        --j->queued_count;
        j->queued_bytes -= e->size;
        unlock(j);
        switch (e->type) {
        case 0: base->events.state(base->events.user, e->state); break;
        case 1: base->events.candidate(base->events.user, (const char *)e->data); break;
        case 2: base->events.gathering_done(base->events.user); break;
        case 3: base->events.recv(base->events.user, e->data, e->size); break;
        }
        release_event(j, e);
    }
}
static int juice_tick(ice_adapter *base) {
    unsigned packets = 256, events = 256; bool more;
    return juice_drain(base, &packets, &events, &more);
}
static ewrtc_socket juice_socket(ice_adapter *base) { (void)base; return NULL; }
static uint64_t juice_deadline(ice_adapter *base) { (void)base; return UINT64_MAX; }
static int juice_timers(ice_adapter *base) { (void)base; return 0; }
static int juice_remote_credentials(ice_adapter *base, const char *ufrag, const char *pwd) {
    juice_adapter *j = (juice_adapter *)base;
    char description[1024];
    int n = snprintf(description, sizeof(description), "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag,
                     pwd);
    if (n < 0 || (size_t)n >= sizeof(description))
        return EWRTC_INVALID;
    return from_juice(juice_set_remote_description(j->agent, description));
}
static int juice_add_candidate(ice_adapter *base, const char *candidate) {
    juice_adapter *j = (juice_adapter *)base;
    /* Chrome hides host IPs behind mDNS names. This libjuice revision does not
     * resolve them; its peer-reflexive checks still establish the IPv4 path. */
    if (strstr(candidate, ".local "))
        return 0;
    char line[EWRTC_MAX_CANDIDATE + 3];
    if (!strncmp(candidate, "a=", 2))
        return from_juice(juice_add_remote_candidate(j->agent, candidate));
    int n = snprintf(line, sizeof(line), "a=%s", candidate);
    if (n < 0 || (size_t)n >= sizeof(line))
        return EWRTC_INVALID;
    return from_juice(juice_add_remote_candidate(j->agent, line));
}
static int juice_end_candidates(ice_adapter *base) {
    juice_adapter *j = (juice_adapter *)base;
    return from_juice(juice_set_remote_gathering_done(j->agent));
}
static int juice_send_packet(ice_adapter *base, const uint8_t *data, size_t size) {
    juice_adapter *j = (juice_adapter *)base;
    return from_juice(juice_send(j->agent, (const char *)data, size));
}
static int juice_selected(ice_adapter *base, char *local, size_t lcap, char *remote, size_t rcap) {
    juice_adapter *j = (juice_adapter *)base;
    return from_juice(juice_get_selected_candidates(j->agent, local, lcap, remote, rcap));
}
static void juice_close(ice_adapter *base) {
    juice_adapter *j = (juice_adapter *)base;
    if (j->agent)
        juice_destroy(j->agent); /* joins producers before freeing queue */
    while (j->head) {
        juice_event *e = j->head;
        j->head = e->next;
        release_event(j, e);
    }
    if (j->mutex)
        base->pal.threads.mutex_destroy(base->pal.threads.ctx, j->mutex);
    ewrtc_free(&base->pal, j);
}
static const ice_ops ops = {juice_start,         juice_tick,           juice_remote_credentials,
                            juice_add_candidate, juice_end_candidates, juice_send_packet,
                            juice_selected,      juice_close, juice_socket, juice_drain,
                            juice_timers, juice_deadline};

ice_adapter *ice_juice_create(const ewrtc_ice_config *config, const ewrtc_ice_events *events, int *error) {
    *error = EWRTC_NOMEM;
    init_juice_logging();
    juice_adapter *j = ewrtc_zalloc(&config->pal, sizeof(*j));
    if (!j)
        return NULL;
    j->base.pal = config->pal;
    j->base.delivery = config->delivery;
    *error = config->pal.threads.mutex_create(config->pal.threads.ctx, &j->mutex);
    if (*error) {
        ewrtc_free(&config->pal, j);
        return NULL;
    }
    *error = EWRTC_IO;
    j->base.ops = &ops;
    j->base.events = *events;
    j->relay_only = config->relay_only != 0;
    juice_turn_server_t turn = {0};
    if (config->turn_host && *config->turn_host) {
        turn.host = config->turn_host;
        turn.port = config->turn_port ? config->turn_port : 3478;
        turn.username = config->turn_username;
        turn.password = config->turn_password;
    }
    juice_config_t jc = {0};
    jc.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    jc.bind_address = "0.0.0.0";
    jc.stun_server_host = config->stun_host;
    jc.stun_server_port = config->stun_port ? config->stun_port : 3478;
    jc.turn_servers = turn.host ? &turn : NULL;
    jc.turn_servers_count = turn.host ? 1 : 0;
    jc.cb_state_changed = state_cb;
    jc.cb_candidate = candidate_cb;
    jc.cb_gathering_done = done_cb;
    jc.cb_recv = recv_cb;
    jc.user_ptr = j;
    j->agent = juice_create(&jc);
    if (!j->agent) {
        juice_close(&j->base);
        return NULL;
    }
    char description[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(j->agent, description, sizeof(description)) < 0 ||
        get_credential(description, "a=ice-ufrag:", j->base.ufrag, sizeof(j->base.ufrag)) ||
        get_credential(description, "a=ice-pwd:", j->base.pwd, sizeof(j->base.pwd))) {
        juice_close(&j->base);
        return NULL;
    }
    *error = EWRTC_OK;
    return &j->base;
}
