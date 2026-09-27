#define _POSIX_C_SOURCE 200809L
#include "session_hub.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static atomic_bool stop_camera;
static void stop_signal(int sig) { (void)sig; stopping = 1; }
static void *camera_main(void *user) {
    session_hub *hub = user;
    while (!atomic_load(&stop_camera)) {
        if (camera_source_run(&stop_camera, session_hub_frame, hub)) session_hub_source_error(hub);
        for (unsigned i = 0; i < 10 && !atomic_load(&stop_camera); ++i) {
            struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
        }
    }
    return NULL;
}
int main(int argc, char **argv) {
    unsigned limit = 4;
    if (argc == 2) {
        char *end; unsigned long value = strtoul(argv[1], &end, 10);
        if (*end || value < 1 || value > 8) return 2;
        limit = (unsigned)value;
    } else if (argc != 1) { fprintf(stderr, "Usage: %s [max-sessions:1..8]\n", argv[0]); return 2; }
    int lock = open("/tmp/ewrtc-camera-mux.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { fprintf(stderr, "Camera WebRTC process is already running\n"); return 1; }
    struct sigaction action = {0}; action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask); sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    atomic_init(&stop_camera, false);
    mux_io *io = mux_io_create(&stopping);
    if (!io) { close(lock); return 1; }
    session_hub *hub = session_hub_create(io, limit);
    if (!hub) { mux_io_destroy(io); close(lock); return 1; }
    pthread_t camera;
    if (pthread_create(&camera, NULL, camera_main, hub)) { session_hub_destroy(hub); mux_io_destroy(io); close(lock); return 1; }
    char ready[128]; snprintf(ready, sizeof(ready), "{\"pid\":%ld,\"max_sessions\":%u}", (long)getpid(), limit);
    mux_io_send(io, 0, "READY", ready);
    mux_record record; int result;
    while (!stopping && (result = mux_io_read(io, &record)) > 0) {
        session_hub_command(hub, &record); free(record.data);
    }
    atomic_store(&stop_camera, true); pthread_join(camera, NULL);
    session_hub_destroy(hub); mux_io_destroy(io); close(lock);
    return stopping ? 0 : 1; /* Broken signaling transport: supervisor restarts. */
}
