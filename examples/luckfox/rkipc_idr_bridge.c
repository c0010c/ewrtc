#define _GNU_SOURCE
/* Optional LD_PRELOAD module for the original rkipc, which owns VENC channel 0.
 * Resolves the installed Rockchip C ABI at runtime; no proprietary library is copied.
 * Official rk_mpi_venc.h: RK_S32 RK_MPI_VENC_RequestIDR(VENC_CHN, RK_BOOL).
 * On this ARM32 firmware all three ABI types are 32-bit integers. RK_FALSE = 0,
 * as used by Luckfox's sample_venc_stresstest.c for requesting the next IDR.
 * The module neither initializes nor destroys the vendor's encoder resources.
 */
#include "idr_protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(int) == 4, "Rockchip ABI needs 32-bit int");
typedef int (*request_idr_fn)(int, int);
static request_idr_fn request_idr;
static int control_fd = -1, lock_fd = -1;
static pthread_t thread;
static atomic_bool stopping;
static bool running, bound;
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void *control_main(void *unused) {
    (void)unused;
    uint64_t last = 0; bool pending = false;
    while (!atomic_load(&stopping)) {
        uint64_t now = now_ms();
        if (pending && (!last || now - last >= EWRTC_IDR_INTERVAL_MS)) {
            int result = request_idr(0, 0);
            fprintf(stderr, "ewrtc IDR: request channel=0 result=%d at_ms=%llu\n", result,
                    (unsigned long long)now);
            last = now; pending = false;
        }
        int timeout = pending ? (int)(EWRTC_IDR_INTERVAL_MS - (now - last)) : 100;
        struct pollfd p = {control_fd, POLLIN, 0};
        int ready = poll(&p, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (ready > 0) {
            /* Bounded drain merges simultaneous viewers; rate limiting also applies to PLI. */
            for (unsigned i = 0; i < 64; ++i) {
                char data[64]; ssize_t n = recv(control_fd, data, sizeof(data), MSG_DONTWAIT);
                if (n < 0) break;
                if (n == sizeof(EWRTC_IDR_REQUEST) &&
                    !memcmp(data, EWRTC_IDR_REQUEST, sizeof(EWRTC_IDR_REQUEST))) pending = true;
            }
        }
    }
    return NULL;
}
__attribute__((destructor)) static void bridge_stop(void) {
    if (running) {
        atomic_store(&stopping, true); pthread_join(thread, NULL); running = false;
    }
    if (control_fd >= 0) { close(control_fd); control_fd = -1; }
    if (bound) { unlink(EWRTC_IDR_SOCKET); bound = false; }
    if (lock_fd >= 0) { close(lock_fd); lock_fd = -1; }
}
__attribute__((constructor)) static void bridge_start(void) {
    /* Shells launched by rkipc may inherit LD_PRELOAD; do nothing without Rockchip. */
    request_idr = (request_idr_fn)dlsym(RTLD_DEFAULT, "RK_MPI_VENC_RequestIDR");
    if (!request_idr) return;
    lock_fd = open(EWRTC_IDR_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) goto fail;
    control_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (control_fd < 0) goto fail;
    struct sockaddr_un address = {0}; address.sun_family = AF_UNIX;
    memcpy(address.sun_path, EWRTC_IDR_SOCKET, sizeof(EWRTC_IDR_SOCKET));
    unlink(EWRTC_IDR_SOCKET); /* Exclusive lock proves no active bridge owns this path. */
    if (bind(control_fd, (const struct sockaddr *)&address, sizeof(address))) goto fail;
    bound = true;
    if (chmod(EWRTC_IDR_SOCKET, 0600)) goto fail;
    atomic_init(&stopping, false);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) goto fail;
    int result = pthread_attr_setstacksize(&attr, 64 * 1024);
    if (!result) result = pthread_create(&thread, &attr, control_main, NULL);
    pthread_attr_destroy(&attr);
    if (result) goto fail;
    running = true;
    fprintf(stderr, "ewrtc IDR: bridge ready on %s\n", EWRTC_IDR_SOCKET);
    return;
fail:
    fprintf(stderr, "ewrtc IDR: bridge unavailable; periodic IDR remains active\n");
    bridge_stop();
}
