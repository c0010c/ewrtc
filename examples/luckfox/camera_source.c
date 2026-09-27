#define _POSIX_C_SOURCE 200809L
#include "camera_source.h"
#include "camera_h264.h"
#include <arpa/inet.h>
#include <errno.h>
#include <mbedtls/base64.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    const atomic_bool *stop;
    unsigned cseq;
    char session[128];
    uint8_t input[8192];
    size_t pos, end;
} rtsp_client;
static uint64_t mono_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int read_bytes(rtsp_client *c, void *buffer, size_t n) {
    uint8_t *out = buffer;
    uint64_t deadline = mono_ms() + 10000;
    while (n && !atomic_load(c->stop)) {
        if (c->pos == c->end) {
            if (mono_ms() >= deadline) return -1;
            struct pollfd p = {c->fd, POLLIN, 0};
            int ready = poll(&p, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) return -1;
            if (!ready) continue;
            ssize_t count = recv(c->fd, c->input, sizeof(c->input), 0);
            if (count <= 0) return -1;
            c->pos = 0; c->end = (size_t)count;
        }
        size_t copy = c->end - c->pos;
        if (copy > n) copy = n;
        memcpy(out, c->input + c->pos, copy);
        c->pos += copy; out += copy; n -= copy;
    }
    return n ? -1 : 0;
}
static int write_bytes(rtsp_client *c, const char *data, size_t n) {
    while (n) {
        ssize_t count = send(c->fd, data, n, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        data += count; n -= (size_t)count;
    }
    return 0;
}
static int header_value(const char *header, const char *name, char *out, size_t cap) {
    size_t length = strlen(name);
    const char *line = header;
    while (line && *line) {
        if (!strncasecmp(line, name, length) && line[length] == ':') {
            line += length + 1; while (*line == ' ') ++line;
            size_t n = strcspn(line, "\r\n");
            if (n >= cap) return -1;
            memcpy(out, line, n); out[n] = 0; return 0;
        }
        line = strstr(line, "\r\n"); if (line) line += 2;
    }
    return -1;
}
static int response(rtsp_client *c, char *body, size_t cap) {
    char header[8192], value[128]; size_t used = 0;
    while (used < sizeof(header) - 1) {
        if (read_bytes(c, header + used, 1)) return -1;
        ++used; header[used] = 0;
        if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
    }
    if (strncmp(header, "RTSP/1.0 200", 12)) {
        fprintf(stderr, "Camera RTSP: %.80s\n", header); return -1;
    }
    size_t length = 0;
    if (!header_value(header, "Content-Length", value, sizeof(value))) {
        char *end; unsigned long parsed = strtoul(value, &end, 10);
        if (*end || parsed >= cap) return -1;
        length = (size_t)parsed;
    }
    if (length && read_bytes(c, body, length)) return -1;
    body[length] = 0;
    if (!header_value(header, "Session", value, sizeof(value))) {
        value[strcspn(value, ";")] = 0;
        snprintf(c->session, sizeof(c->session), "%s", value);
    }
    return 0;
}
static int request(rtsp_client *c, const char *method, const char *url,
                   const char *extra, char *body, size_t cap) {
    char request_data[2048], session_header[160] = "";
    if (*c->session) snprintf(session_header, sizeof(session_header), "Session: %s\r\n", c->session);
    int n = snprintf(request_data, sizeof(request_data),
        "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: ewrtc-camera\r\n%s%s\r\n",
        method, url, ++c->cseq, session_header, extra);
    if (n < 0 || (size_t)n >= sizeof(request_data) || write_bytes(c, request_data, (size_t)n)) return -1;
    return response(c, body, cap);
}
static void sdp_parameters(camera_h264 *h, const char *sdp) {
    const char *p = strstr(sdp, "sprop-parameter-sets=");
    if (!p) return;
    p += strlen("sprop-parameter-sets=");
    const char *comma = strchr(p, ',');
    if (!comma) return;
    size_t len;
    if (!mbedtls_base64_decode(h->sps, sizeof(h->sps), &len, (const unsigned char *)p, (size_t)(comma - p))) h->sps_size = len;
    p = comma + 1;
    if (!mbedtls_base64_decode(h->pps, sizeof(h->pps), &len, (const unsigned char *)p, strcspn(p, ";\r\n "))) h->pps_size = len;
}
int camera_source_run(const atomic_bool *stop, camera_frame_callback callback, void *user) {
    const char *url = "rtsp://127.0.0.1/live/0";
    rtsp_client c = {.fd = -1, .stop = stop};
    camera_h264 *h = calloc(1, sizeof(*h));
    uint8_t *packet = malloc(65536);
    char body[8192], track_url[1024];
    int result = -1;
    if (!h || !packet) goto done;
    h->frame = callback; h->user = user;
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd < 0) goto done;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(554);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c.fd, (struct sockaddr *)&address, sizeof(address))) goto done;
    if (request(&c, "DESCRIBE", url, "Accept: application/sdp\r\n", body, sizeof(body))) goto done;
    sdp_parameters(h, body);
    const char *video = strstr(body, "m=video ");
    const char *control = video ? strstr(video, "a=control:") : NULL;
    if (!control || !strstr(video, "H264/90000")) goto done;
    control += strlen("a=control:");
    size_t length = strcspn(control, "\r\n");
    if (length > 500) goto done;
    if (!strncmp(control, "rtsp://", 7)) snprintf(track_url, sizeof(track_url), "%.*s", (int)length, control);
    else snprintf(track_url, sizeof(track_url), "%s/%.*s", url, (int)length, control);
    if (request(&c, "SETUP", track_url, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", body, sizeof(body)) ||
        request(&c, "PLAY", url, "Range: npt=0.000-\r\n", body, sizeof(body))) goto done;
    fprintf(stderr, "Camera source connected: %s (hardware H.264)\n", url);
    while (!atomic_load(stop)) {
        uint8_t header[4];
        if (read_bytes(&c, header, 4) || header[0] != '$') goto done;
        size_t n = ((size_t)header[2] << 8) | header[3];
        if (read_bytes(&c, packet, n)) goto done;
        if (header[1] == 0) camera_h264_packet(h, packet, n);
    }
    result = 0;
done:
    if (c.fd >= 0) close(c.fd);
    free(packet); free(h);
    return atomic_load(stop) ? 0 : result;
}
