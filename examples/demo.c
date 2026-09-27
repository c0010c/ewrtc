#include "ewrtc.h"
#include "ewrtc/platform/linux.h"
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ewrtc_session *session;
static pthread_mutex_t output_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool connected, stop_media, keyframe_requested;
static FILE *received_audio, *received_video;
static atomic_bool local_offerer;
static const char *video_path;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
static void write_frame(const char *name, const void *data, size_t len) {
    pthread_mutex_lock(&output_mu);
    printf("%s %zu\n", name, len);
    if (len) fwrite(data, 1, len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&output_mu);
}
static void state_cb(ewrtc_session *s, ewrtc_state state, void *user) {
    (void)s; (void)user;
    atomic_store(&connected, state == EWRTC_CONNECTED);
    char text[32];
    int n = snprintf(text, sizeof(text), "%d", state);
    write_frame("STATE", text, (size_t)n);
}
static void answer_cb(ewrtc_session *s, const char *sdp, void *user) {
    (void)s; (void)user;
    write_frame(atomic_load(&local_offerer) ? "OFFER" : "ANSWER", sdp, strlen(sdp));
}
static void candidate_cb(ewrtc_session *s, const char *candidate, void *user) {
    (void)s; (void)user;
    write_frame("CANDIDATE", candidate, strlen(candidate));
}
static void done_cb(ewrtc_session *s, void *user) {
    (void)s; (void)user;
    write_frame("DONE", NULL, 0);
}
static void audio_cb(ewrtc_session *s, const uint8_t *data, size_t length,
                     uint32_t timestamp, uint16_t sequence, void *user) {
    (void)s; (void)user;
    if (!received_audio) return;
    uint8_t header[10] = {(uint8_t)(length >> 8), (uint8_t)length,
        (uint8_t)(timestamp >> 24), (uint8_t)(timestamp >> 16),
        (uint8_t)(timestamp >> 8), (uint8_t)timestamp,
        (uint8_t)(sequence >> 8), (uint8_t)sequence, 0, 0};
    fwrite(header, 1, sizeof(header), received_audio);
    fwrite(data, 1, length, received_audio);
}
static void video_cb(ewrtc_session *s, const uint8_t *data, size_t length,
                     uint32_t timestamp, int keyframe, void *user) {
    (void)s; (void)timestamp; (void)keyframe; (void)user;
    if (received_video) fwrite(data, 1, length, received_video);
}
static void keyframe_cb(ewrtc_session *s, void *user) {
    (void)s; (void)user;
    atomic_store(&keyframe_requested, true);
}
static void error_cb(ewrtc_session *s, ewrtc_result code,
                     const char *detail, void *user) {
    (void)s; (void)user;
    fprintf(stderr, "SDK error %d: %s\n", code, detail);
    write_frame("ERROR", detail, strlen(detail));
}
static uint32_t read_be32(FILE *f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}
static int is_keyframe(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + 4 < size; ++i)
        if (data[i] == 0 && data[i + 1] == 0 &&
            ((data[i + 2] == 1 && (data[i + 3] & 31) == 5) ||
             (data[i + 2] == 0 && data[i + 3] == 1 &&
              (data[i + 4] & 31) == 5))) return 1;
    return 0;
}
static void *media_main(void *arg) {
    (void)arg;
    FILE *video = fopen(video_path, "rb");
    if (!video) { perror(video_path); return NULL; }
    int opus_error;
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if (!encoder || opus_error != OPUS_OK) { fclose(video); return NULL; }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
    int16_t pcm[960];
    uint8_t opus[512];
    /* Embedded pthread stacks can be smaller than one maximum video AU. */
    const size_t frame_capacity = 2 * 1024 * 1024;
    uint8_t *frame = malloc(frame_capacity);
    if (!frame) { opus_encoder_destroy(encoder); fclose(video); return NULL; }
    uint64_t audio_index = 0, video_index = 0, start = 0;
    unsigned phase = 0;
    while (!atomic_load(&stop_media)) {
        if (!atomic_load(&connected)) {
            start = 0;
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
            continue;
        }
        if (!start) start = now_us();
        uint64_t elapsed = now_us() - start;
        if (elapsed >= audio_index * 20000) {
            for (int i = 0; i < 960; ++i) {
                /* 600 Hz square wave remains recognizable after Opus encoding. */
                pcm[i] = (phase++ % 80) < 40 ? 5000 : -5000;
            }
            int count = opus_encode(encoder, pcm, 960, opus, sizeof(opus));
            if (count > 0) ewrtc_session_send_audio(session, opus, (size_t)count,
                                                    audio_index * 20000);
            audio_index++;
        }
        if (elapsed >= video_index * 33333) {
            if (atomic_exchange(&keyframe_requested, false)) rewind(video);
            uint32_t length = read_be32(video);
            if (!length) { rewind(video); length = read_be32(video); }
            if (length && length <= frame_capacity &&
                fread(frame, 1, length, video) == length)
                ewrtc_session_send_video(session, frame, length,
                                         video_index * 33333,
                                         is_keyframe(frame, length));
            video_index++;
        }
        struct timespec pause = {0, 2000000};
        nanosleep(&pause, NULL);
    }
    opus_encoder_destroy(encoder);
    free(frame);
    fclose(video);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <native|juice> <openssl|mbedtls> <video.frames> [stun-host] [turn-host] [turn-user] [turn-pass] [relay-only] [turn-port] [queue-limit-bytes]\n", argv[0]);
        return 2;
    }
    video_path = argv[3];
    received_audio = fopen("received.opuspkts", "wb");
    received_video = fopen("received.h264", "wb");
    ewrtc_session_config config;
    ewrtc_session_config_init(&config);
    ewrtc_context_config cc; ewrtc_context_config_init(&cc);
    cc.pal = *ewrtc_pal_linux();
    ewrtc_context *context;
    if (ewrtc_context_create(&cc, &context)) return 1;
    config.ice_backend = !strcmp(argv[1], "juice") ? EWRTC_ICE_LIBJUICE : EWRTC_ICE_NATIVE;
    config.dtls_backend = !strcmp(argv[2], "mbedtls") ? EWRTC_DTLS_MBEDTLS : EWRTC_DTLS_OPENSSL;
    config.crypto_backend = !strcmp(argv[2], "mbedtls") ? EWRTC_CRYPTO_MBEDTLS : EWRTC_CRYPTO_OPENSSL;
    config.stun_host = argc > 4 && argv[4][0] ? argv[4] : NULL;
    config.turn_host = argc > 5 && argv[5][0] ? argv[5] : NULL;
    config.turn_username = argc > 6 && argv[6][0] ? argv[6] : NULL;
    config.turn_password = argc > 7 && argv[7][0] ? argv[7] : NULL;
    config.relay_only = argc > 8 ? atoi(argv[8]) : 0;
    config.turn_port = argc > 9 ? (uint16_t)atoi(argv[9]) : 3478;
    config.send_queue_limit_bytes = argc > 10 ? (size_t)strtoull(argv[10], NULL, 10) : 0;
    ewrtc_callbacks callbacks = {.on_state = state_cb, .on_local_sdp = answer_cb,
        .on_local_candidate = candidate_cb, .on_gathering_done = done_cb,
        .on_audio = audio_cb, .on_keyframe_request = keyframe_cb, .on_error = error_cb,
        .on_video = video_cb};
    if (ewrtc_session_create(context, &config, &callbacks, NULL, &session) != EWRTC_OK)
        return 1;
    pthread_t media;
    if (pthread_create(&media, NULL, media_main, NULL)) return 1;
    char *line = NULL;
    size_t line_cap = 0;
    while (getline(&line, &line_cap, stdin) > 0) {
        char command[32];
        size_t length;
        if (sscanf(line, "%31s %zu", command, &length) != 2 ||
            length > 65536) break;
        char *data = malloc(length + 1);
        if (!data) break;
        if (fread(data, 1, length, stdin) != length) { free(data); break; }
        data[length] = 0;
        if (!strcmp(command, "CREATE_OFFER")) {
            atomic_store(&local_offerer, true);
            ewrtc_session_create_offer(session);
        }
        else if (!strcmp(command, "ANSWER")) ewrtc_session_set_remote_answer(session, data);
        else if (!strcmp(command, "OFFER")) ewrtc_session_set_remote_offer(session, data);
        else if (!strcmp(command, "CANDIDATE")) ewrtc_session_add_remote_candidate(session, data);
        else if (!strcmp(command, "END")) ewrtc_session_end_remote_candidates(session);
        else if (!strcmp(command, "STATS")) {
            ewrtc_stats stats;
            ewrtc_session_get_stats(session, &stats);
            char report[1024];
            int n = snprintf(report, sizeof(report),
                "state=%d sent_video=%lu sent_audio=%lu recv_audio=%lu recv_video=%lu recv_frames=%lu nack=%lu rtx=%lu pli=%lu backpressure=%lu queue=%zu cache=%zu local=%s remote=%s",
                stats.state, (unsigned long)stats.sent_video_packets,
                (unsigned long)stats.sent_audio_packets,
                (unsigned long)stats.received_audio_packets,
                (unsigned long)stats.received_video_packets,
                (unsigned long)stats.received_video_frames,
                (unsigned long)stats.nack_requests,
                (unsigned long)stats.rtx_packets,
                (unsigned long)stats.pli_requests,
                (unsigned long)stats.backpressure_count,
                stats.send_queue_bytes, stats.retransmit_cache_bytes,
                stats.local_candidate, stats.remote_candidate);
            write_frame("STATS", report, (size_t)n);
        }
        free(data);
    }
    free(line);
    atomic_store(&stop_media, true);
    pthread_join(media, NULL);
    ewrtc_session_destroy(session);
    ewrtc_context_destroy(context);
    if (received_audio) fclose(received_audio);
    if (received_video) fclose(received_video);
    return 0;
}
