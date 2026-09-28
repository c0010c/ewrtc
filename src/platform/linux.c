#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "ewrtc/platform/linux.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <limits.h>

static int io_error(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK ? EWRTC_AGAIN : EWRTC_IO;
}
static void *allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}
static void *resize(void *ctx, void *p, size_t n) {
    (void)ctx;
    return realloc(p, n);
}
static void release(void *ctx, void *p) {
    (void)ctx;
    free(p);
}
static uint64_t mono(void *ctx) {
    (void)ctx;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t utc(void *ctx) {
    (void)ctx;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static int random_bytes(void *ctx, void *out, size_t n) {
    (void)ctx;
    uint8_t *p = out;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return EWRTC_SECURITY;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
static void log_write(void *ctx, int level, const char *s) {
    (void)ctx;
    (void)level;
    fprintf(stderr, "%s\n", s);
}
static int thread_create(void *ctx, void *(*fn)(void *), void *arg, ewrtc_thread *out) {
    (void)ctx;
    *out = NULL;
    pthread_t *t = malloc(sizeof(*t));
    if (!t)
        return EWRTC_NOMEM;
    if (pthread_create(t, NULL, fn, arg)) {
        free(t);
        return EWRTC_IO;
    }
    *out = t;
    return 0;
}
static int thread_join(void *ctx, ewrtc_thread t) {
    (void)ctx;
    int r = pthread_join(*(pthread_t *)t, NULL);
    if (!r)
        free(t);
    return r ? EWRTC_IO : 0;
}
static int thread_current(void *ctx, ewrtc_thread t) {
    (void)ctx;
    return pthread_equal(pthread_self(), *(pthread_t *)t);
}
static int mutex_create(void *ctx, ewrtc_mutex *out) {
    (void)ctx;
    *out = NULL;
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m)
        return EWRTC_NOMEM;
    if (pthread_mutex_init(m, NULL)) {
        free(m);
        return EWRTC_IO;
    }
    *out = m;
    return 0;
}
static void mutex_destroy(void *ctx, ewrtc_mutex m) {
    (void)ctx;
    if (m) {
        pthread_mutex_destroy(m);
        free(m);
    }
}
static void mutex_lock(void *ctx, ewrtc_mutex m) {
    (void)ctx;
    pthread_mutex_lock(m);
}
static void mutex_unlock(void *ctx, ewrtc_mutex m) {
    (void)ctx;
    pthread_mutex_unlock(m);
}
static int condition_create(void *ctx, ewrtc_condition *out) {
    (void)ctx;
    *out = NULL;
    pthread_cond_t *c = malloc(sizeof(*c));
    if (!c)
        return EWRTC_NOMEM;
    pthread_condattr_t a;
    if (pthread_condattr_init(&a)) {
        free(c);
        return EWRTC_IO;
    }
    int r = pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    if (!r)
        r = pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
    if (r) {
        free(c);
        return EWRTC_IO;
    }
    *out = c;
    return 0;
}
static void condition_destroy(void *ctx, ewrtc_condition c) {
    (void)ctx;
    if (c) {
        pthread_cond_destroy(c);
        free(c);
    }
}
static void condition_signal(void *ctx, ewrtc_condition c) {
    (void)ctx;
    pthread_cond_signal(c);
}
static int condition_wait(void *ctx, ewrtc_condition c, ewrtc_mutex m, uint32_t ms) {
    (void)ctx;
    if (ms == UINT32_MAX) return pthread_cond_wait(c, m) ? EWRTC_IO : 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000;
    }
    int r = pthread_cond_timedwait(c, m, &t);
    return r == ETIMEDOUT ? EWRTC_TIMEOUT : r ? EWRTC_IO : 0;
}
static struct sockaddr_in native_addr(const ewrtc_address *a) {
    struct sockaddr_in n = {0};
    n.sin_family = AF_INET;
    n.sin_port = htons(a->port);
    n.sin_addr.s_addr = htonl(a->ipv4);
    return n;
}
static ewrtc_address portable_addr(const struct sockaddr_in *a) {
    return (ewrtc_address){ntohl(a->sin_addr.s_addr), ntohs(a->sin_port)};
}
static int udp_open(void *ctx, const ewrtc_address *a, ewrtc_socket *out) {
    (void)ctx;
    *out = NULL;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return EWRTC_IO;
    struct sockaddr_in n = native_addr(a);
    if (bind(fd, (const struct sockaddr *)&n, sizeof(n))) {
        close(fd);
        return EWRTC_IO;
    }
    int *p = malloc(sizeof(*p));
    if (!p) {
        close(fd);
        return EWRTC_NOMEM;
    }
    *p = fd;
    *out = p;
    return 0;
}
static void udp_close(void *ctx, ewrtc_socket s) {
    (void)ctx;
    if (s) {
        close(*(int *)s);
        free(s);
    }
}
static int udp_local(void *ctx, ewrtc_socket s, ewrtc_address *a) {
    (void)ctx;
    struct sockaddr_in n;
    socklen_t len = sizeof(n);
    if (getsockname(*(int *)s, (struct sockaddr *)&n, &len))
        return EWRTC_IO;
    *a = portable_addr(&n);
    return 0;
}
static int udp_send(void *ctx, ewrtc_socket s, const ewrtc_address *a, const void *data, size_t n) {
    (void)ctx;
    struct sockaddr_in to = native_addr(a);
    ssize_t r;
    do {
        r = sendto(*(int *)s, data, n, 0, (const struct sockaddr *)&to, sizeof(to));
    } while (r < 0 && errno == EINTR);
    return r < 0 ? io_error() : (size_t)r == n ? 0 : EWRTC_IO;
}
static int udp_receive(void *ctx, ewrtc_socket s, ewrtc_address *a, void *data, size_t cap,
                       size_t *size) {
    (void)ctx;
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    ssize_t r;
    *size = 0;
    do {
        r = recvfrom(*(int *)s, data, cap, MSG_TRUNC, (struct sockaddr *)&from, &len);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return io_error();
    if ((size_t)r > cap)
        return EWRTC_INVALID;
    *a = portable_addr(&from);
    *size = (size_t)r;
    return 0;
}
static int resolve(void *ctx, const char *host, uint16_t port, ewrtc_address *a) {
    (void)ctx;
    if (!host || !*host)
        return EWRTC_INVALID;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *result = NULL;
    if (getaddrinfo(host, service, &hint, &result))
        return EWRTC_IO;
    *a = portable_addr((const struct sockaddr_in *)result->ai_addr);
    freeaddrinfo(result);
    return 0;
}
static int interfaces(void *ctx, ewrtc_address *out, size_t cap, size_t *count) {
    (void)ctx;
    *count = 0;
    if (!cap)
        return EWRTC_INVALID;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list))
        return EWRTC_IO;
    for (struct ifaddrs *i = list; i && *count < cap; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
            !(i->ifa_flags & IFF_LOOPBACK)) {
            out[*count] = portable_addr((const struct sockaddr_in *)i->ifa_addr);
            out[(*count)++].port = 0;
        }
    }
    freeifaddrs(list);
    if (!*count)
        out[(*count)++] = (ewrtc_address){0x7f000001, 0};
    return 0;
}
typedef struct { int epoll_fd, wake_fd; } linux_waiter;
static void events_destroy(void *ctx, ewrtc_waiter handle) {
    (void)ctx;
    linux_waiter *w = handle;
    if (!w) return;
    if (w->wake_fd >= 0) close(w->wake_fd);
    if (w->epoll_fd >= 0) close(w->epoll_fd);
    free(w);
}
static int events_create(void *ctx, ewrtc_waiter *out) {
    *out = NULL;
    linux_waiter *w = malloc(sizeof(*w));
    if (!w) return EWRTC_NOMEM;
    w->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    w->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0};
    if (w->epoll_fd < 0 || w->wake_fd < 0 ||
        epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->wake_fd, &event)) {
        events_destroy(ctx, w); return EWRTC_IO;
    }
    *out = w;
    return EWRTC_OK;
}
static int events_add(void *ctx, ewrtc_waiter handle, ewrtc_socket socket, uint64_t token) {
    (void)ctx;
    linux_waiter *w = handle;
    if (!token) return EWRTC_INVALID;
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = token};
    return epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, *(int *)socket, &event) ? EWRTC_IO : 0;
}
static int events_remove(void *ctx, ewrtc_waiter handle, ewrtc_socket socket) {
    (void)ctx;
    linux_waiter *w = handle;
    return epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, *(int *)socket, NULL) ? EWRTC_IO : 0;
}
static int events_wake(void *ctx, ewrtc_waiter handle) {
    (void)ctx;
    linux_waiter *w = handle;
    uint64_t one = 1;
    ssize_t result;
    do { result = write(w->wake_fd, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    return result == sizeof(one) || (result < 0 && errno == EAGAIN) ? 0 : EWRTC_IO;
}
static int events_wait(void *ctx, ewrtc_waiter handle, uint64_t *tokens, size_t cap,
                       size_t *count, uint32_t timeout) {
    (void)ctx;
    linux_waiter *w = handle;
    struct epoll_event events[64];
    *count = 0;
    if (!cap) return EWRTC_INVALID;
    int ms = timeout == UINT32_MAX ? -1 : timeout > INT_MAX ? INT_MAX : (int)timeout;
    int n = epoll_wait(w->epoll_fd, events, cap < 64 ? (int)cap : 64, ms);
    if (n < 0) return errno == EINTR ? 0 : EWRTC_IO;
    for (int i = 0; i < n; ++i) {
        if (events[i].data.u64) tokens[(*count)++] = events[i].data.u64;
        else {
            uint64_t value;
            ssize_t result;
            do { result = read(w->wake_fd, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
        }
    }
    return 0;
}
const ewrtc_pal *ewrtc_pal_linux(void) {
    static const ewrtc_pal p = {.memory = {NULL, allocate, resize, release},
                                .clock = {NULL, mono, utc},
                                .random = {NULL, random_bytes},
                                .threads = {NULL, thread_create, thread_join, thread_current,
                                            mutex_create, mutex_destroy, mutex_lock, mutex_unlock,
                                            condition_create, condition_destroy, condition_signal,
                                            condition_wait},
                                .network = {NULL, udp_open, udp_close, udp_local, udp_send,
                                            udp_receive, resolve, interfaces},
                                .log = {NULL, log_write},
                                .events = {NULL, events_create, events_destroy, events_add,
                                           events_remove, events_wait, events_wake}};
    return &p;
}
