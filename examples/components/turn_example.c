#define _POSIX_C_SOURCE 200809L
#include "turn/turn.h"
#include "ewrtc/platform/linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { bool received; } example;
static void receive(void *ctx, const ewrtc_address *peer, const uint8_t *data, size_t size) {
    example *e = ctx;
    char ip[16];
    ewrtc_address_format(peer, ip, sizeof(ip));
    printf("Received %zu bytes from %s:%u\n", size, ip, peer->port);
    e->received = size == 16 && !memcmp(data, "ewrtc TURN probe", 16);
}
static unsigned port_number(const char *text) {
    char *end; unsigned long value = strtoul(text, &end, 10);
    return *text && !*end && value && value <= 65535 ? (unsigned)value : 0;
}
int main(int argc, char **argv) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: %s TURN_HOST USER PASSWORD PEER_IP PEER_PORT [TURN_PORT]\n"
                        "The peer must echo UDP packets.\n", argv[0]);
        return 2;
    }
    unsigned peer_port = port_number(argv[5]), server_port = argc == 7 ? port_number(argv[6]) : 3478;
    ewrtc_address peer, bind = {0};
    if (!peer_port || !server_port || ewrtc_address_parse(argv[4], (uint16_t)peer_port, &peer))
        return 2;
    ewrtc_pal pal = *ewrtc_pal_linux();
    ewrtc_socket socket = NULL;
    ewrtc_turn *turn = NULL;
    example e = {0};
    int result = pal.network.udp_open(pal.network.ctx, &bind, &socket);
    if (result) goto done;
    ewrtc_turn_config c = {.pal = pal, .socket = socket,
        .crypto_backend = ewrtc_crypto_available(EWRTC_CRYPTO_OPENSSL) ? EWRTC_CRYPTO_OPENSSL : EWRTC_CRYPTO_MBEDTLS,
        .server_host = argv[1], .server_port = (uint16_t)server_port,
        .username = argv[2], .password = argv[3], .recv = receive, .user = &e};
    if ((result = ewrtc_turn_create(&c, &turn)) ||
        (result = ewrtc_turn_add_permission(turn, &peer)) || (result = ewrtc_turn_start(turn)))
        goto done;
    uint64_t deadline = ewrtc_now_ms(&pal) + 10000;
    bool sent = false, printed = false;
    while (!e.received && ewrtc_now_ms(&pal) < deadline) {
        for (unsigned i = 0; i < 64; ++i) {
            uint8_t bytes[2048]; size_t size; ewrtc_address from;
            result = pal.network.udp_receive(pal.network.ctx, socket, &from, bytes, sizeof(bytes), &size);
            if (result == EWRTC_AGAIN) break;
            if (result == EWRTC_INVALID) continue;
            if (result) goto done;
            result = ewrtc_turn_receive(turn, &from, bytes, size);
            if (result && result != EWRTC_AGAIN && result != EWRTC_INVALID && result != EWRTC_SECURITY)
                goto done;
        }
        if ((result = ewrtc_turn_tick(turn))) goto done;
        ewrtc_turn_status status;
        ewrtc_turn_get_status(turn, &status);
        if (!printed && status.state == EWRTC_TURN_READY) {
            char ip[16]; ewrtc_address_format(&status.relayed, ip, sizeof(ip));
            printf("Relay allocated: %s:%u\n", ip, status.relayed.port); printed = true;
        }
        if (!sent && ewrtc_turn_has_permission(turn, &peer)) {
            result = ewrtc_turn_send(turn, &peer, (const uint8_t *)"ewrtc TURN probe", 16);
            if (!result) sent = true;
            else if (result != EWRTC_AGAIN) goto done;
        }
        const struct timespec wait = {0, 20000000}; nanosleep(&wait, NULL);
    }
    result = e.received ? EWRTC_OK : EWRTC_TIMEOUT;
done:
    ewrtc_turn_destroy(turn); /* Best-effort release while socket is still valid. */
    if (socket) pal.network.udp_close(pal.network.ctx, socket);
    if (result) fprintf(stderr, "TURN example failed: %d\n", result);
    return result ? 1 : 0;
}
