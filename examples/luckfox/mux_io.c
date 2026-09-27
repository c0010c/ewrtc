#define _POSIX_C_SOURCE 200809L
#include "mux_io.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PAYLOAD 65536
#define MAX_OUTPUT (256 * 1024)
typedef struct message { struct message *next; size_t size; char data[]; } message;
struct mux_io {
    const volatile sig_atomic_t *stop;
    atomic_bool failed, stopping;
    pthread_mutex_t mu;
    pthread_cond_t ready;
    pthread_t writer;
    message *head, *tail;
    size_t bytes, count;
    int old_flags;
};
static bool stopping(mux_io *io) {
    return *io->stop || atomic_load(&io->stopping) || atomic_load(&io->failed);
}
static void *writer_main(void *user) {
    mux_io *io = user;
    for (;;) {
        pthread_mutex_lock(&io->mu);
        while (!io->head && !atomic_load(&io->stopping)) pthread_cond_wait(&io->ready, &io->mu);
        if (atomic_load(&io->stopping)) { pthread_mutex_unlock(&io->mu); break; }
        message *m = io->head;
        io->head = m->next;
        if (!io->head) io->tail = NULL;
        pthread_mutex_unlock(&io->mu);
        size_t offset = 0;
        while (offset < m->size && !stopping(io)) {
            struct pollfd p = {STDOUT_FILENO, POLLOUT, 0};
            int ready = poll(&p, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (!ready) continue;
            if (ready < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { atomic_store(&io->failed, true); break; }
            ssize_t n = write(STDOUT_FILENO, m->data + offset, m->size - offset);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) { atomic_store(&io->failed, true); break; }
            offset += (size_t)n;
        }
        pthread_mutex_lock(&io->mu);
        io->bytes -= m->size; --io->count;
        pthread_mutex_unlock(&io->mu);
        free(m);
        if (stopping(io)) break;
    }
    return NULL;
}
mux_io *mux_io_create(const volatile sig_atomic_t *stop) {
    mux_io *io = calloc(1, sizeof(*io));
    if (!io) return NULL;
    io->stop = stop;
    atomic_init(&io->failed, false); atomic_init(&io->stopping, false);
    io->old_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (io->old_flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, io->old_flags | O_NONBLOCK)) { free(io); return NULL; }
    if (pthread_mutex_init(&io->mu, NULL)) goto flags;
    if (pthread_cond_init(&io->ready, NULL)) goto mutex;
    if (pthread_create(&io->writer, NULL, writer_main, io)) goto condition;
    return io;
condition: pthread_cond_destroy(&io->ready);
mutex: pthread_mutex_destroy(&io->mu);
flags: fcntl(STDOUT_FILENO, F_SETFL, io->old_flags); free(io); return NULL;
}
void mux_io_send(mux_io *io, uint32_t id, const char *kind, const char *payload) {
    if (stopping(io)) return;
    size_t length = strlen(payload);
    char header[96];
    int n = snprintf(header, sizeof(header), "%u %s %zu\n", id, kind, length);
    if (length > MAX_PAYLOAD || n < 0 || (size_t)n >= sizeof(header)) { atomic_store(&io->failed, true); return; }
    size_t bytes = (size_t)n + length;
    message *m = malloc(sizeof(*m) + bytes);
    if (!m) { atomic_store(&io->failed, true); return; }
    m->next = NULL; m->size = bytes;
    memcpy(m->data, header, (size_t)n); memcpy(m->data + n, payload, length);
    pthread_mutex_lock(&io->mu);
    if (io->count >= 128 || bytes > MAX_OUTPUT - io->bytes) {
        atomic_store(&io->failed, true); free(m);
    } else {
        if (io->tail) io->tail->next = m; else io->head = m;
        io->tail = m; io->bytes += bytes; ++io->count;
        pthread_cond_signal(&io->ready);
    }
    pthread_mutex_unlock(&io->mu);
}
static int read_exact(mux_io *io, char *data, size_t size) {
    while (size && !stopping(io)) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&p, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (!ready) continue;
        ssize_t n = read(STDIN_FILENO, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        data += n; size -= (size_t)n;
    }
    return size ? -1 : 1;
}
int mux_io_read(mux_io *io, mux_record *record) {
    char header[128]; size_t used = 0;
    memset(record, 0, sizeof(*record));
    do {
        if (used == sizeof(header) - 1) return -1;
        int result = read_exact(io, header + used, 1);
        if (result <= 0) return used ? -1 : result;
    } while (header[used++] != '\n');
    header[used] = 0;
    unsigned long long id, length; int end = 0;
    if (sscanf(header, "%llu %31[A-Z_] %llu%n", &id, record->kind, &length, &end) != 3 ||
        header[end] != '\n' || header[end + 1] || id > UINT32_MAX || length > MAX_PAYLOAD) return -1;
    record->id = (uint32_t)id; record->length = (size_t)length;
    record->data = malloc(record->length + 1);
    if (!record->data) return -1;
    if (read_exact(io, record->data, record->length) != 1 || memchr(record->data, 0, record->length)) {
        free(record->data); record->data = NULL; return -1;
    }
    record->data[record->length] = 0;
    return 1;
}
void mux_io_destroy(mux_io *io) {
    if (!io) return;
    atomic_store(&io->stopping, true);
    pthread_mutex_lock(&io->mu); pthread_cond_signal(&io->ready); pthread_mutex_unlock(&io->mu);
    pthread_join(io->writer, NULL);
    while (io->head) { message *m = io->head; io->head = m->next; free(m); }
    pthread_cond_destroy(&io->ready); pthread_mutex_destroy(&io->mu);
    fcntl(STDOUT_FILENO, F_SETFL, io->old_flags);
    free(io);
}
