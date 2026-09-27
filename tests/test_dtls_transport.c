#define _POSIX_C_SOURCE 200809L
#include "dtls/dtls.h"
#include "ewrtc/platform/linux.h"
#include <assert.h>
#include <string.h>
#include <time.h>

#define PACKET_CAPACITY 16u
#define DTLS_RECORD_HEADER_SIZE 13u

typedef struct {
    ewrtc_dtls *dtls;
    unsigned head, count, blocked, sent;
    bool dropped;
    size_t sizes[PACKET_CAPACITY], retry_size;
    uint8_t packets[PACKET_CAPACITY][EWRTC_MTU], retry[EWRTC_MTU];
} endpoint;

static int send_packet(void *ctx, const uint8_t *data, size_t size) {
    endpoint *e = ctx;
    assert(size > 0 && size <= EWRTC_MTU);
    /* Every callback must contain complete DTLS records within one datagram. */
    for (size_t off = 0; off < size;) {
        assert(size - off >= DTLS_RECORD_HEADER_SIZE);
        size_t length = ((size_t)data[off + 11] << 8) | data[off + 12];
        off += DTLS_RECORD_HEADER_SIZE + length;
        assert(off <= size);
    }
    if (e->retry_size)
        assert(size == e->retry_size && !memcmp(data, e->retry, size));
    if (e->blocked < 2) {
        memcpy(e->retry, data, size);
        e->retry_size = size;
        return ++e->blocked == 1 ? EWRTC_AGAIN : EWRTC_BACKPRESSURE;
    }
    e->retry_size = 0;
    if (!e->dropped) {
        e->dropped = true; /* Lose the first datagram after unblocking. */
        return 0;
    }
    assert(e->count < PACKET_CAPACITY);
    unsigned slot = (e->head + e->count) % PACKET_CAPACITY;
    ++e->count;
    ++e->sent;
    memcpy(e->packets[slot], data, size);
    e->sizes[slot] = size;
    return 0;
}

static void assert_retryable_or_ok(int result) {
    assert(result == 0 || result == EWRTC_AGAIN || result == EWRTC_BACKPRESSURE);
}

static void deliver(endpoint *from, endpoint *to) {
    while (from->count) {
        unsigned slot = from->head;
        from->head = (from->head + 1) % PACKET_CAPACITY;
        --from->count;
        assert_retryable_or_ok(ewrtc_dtls_receive(to->dtls, from->packets[slot], from->sizes[slot]));
    }
}

int main(void) {
    const ewrtc_pal *pal = ewrtc_pal_linux();
    endpoint client = {0}, server = {0};
    ewrtc_dtls_config config = {.pal = *pal, .backend = EWRTC_DTLS_OPENSSL,
                               .send = send_packet, .user = &client};
    assert(!ewrtc_dtls_create(&config, &client.dtls));
    config.user = &server;
    assert(!ewrtc_dtls_create(&config, &server.dtls));
    assert(!ewrtc_dtls_set_peer(client.dtls, ewrtc_dtls_fingerprint(server.dtls), true));
    assert(!ewrtc_dtls_set_peer(server.dtls, ewrtc_dtls_fingerprint(client.dtls), false));
    assert_retryable_or_ok(ewrtc_dtls_start(server.dtls));
    assert(ewrtc_dtls_start(client.dtls) == EWRTC_AGAIN);
    uint64_t end = ewrtc_now_ms(pal) + 10000;
    while (!ewrtc_dtls_connected(client.dtls) || !ewrtc_dtls_connected(server.dtls)) {
        assert(ewrtc_now_ms(pal) < end);
        deliver(&client, &server);
        deliver(&server, &client);
        if (ewrtc_dtls_next_deadline(client.dtls) <= ewrtc_now_ms(pal))
            assert_retryable_or_ok(ewrtc_dtls_tick(client.dtls));
        if (ewrtc_dtls_next_deadline(server.dtls) <= ewrtc_now_ms(pal))
            assert_retryable_or_ok(ewrtc_dtls_tick(server.dtls));
        struct timespec pause = {0, 5000000};
        nanosleep(&pause, NULL);
    }
    uint8_t client_keys[60], server_keys[60];
    assert(!ewrtc_dtls_export_keys(client.dtls, client_keys));
    assert(!ewrtc_dtls_export_keys(server.dtls, server_keys));
    assert(!memcmp(client_keys, server_keys, sizeof(client_keys)));
    assert(client.blocked == 2 && server.blocked == 2);
    assert(client.dropped && server.dropped && client.sent && server.sent);
    ewrtc_dtls_destroy(client.dtls);
    ewrtc_dtls_destroy(server.dtls);
    return 0;
}
