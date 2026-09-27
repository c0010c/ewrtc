#include "openssl_bio.h"
#include <string.h>

/* Preserve packet boundaries, including when OpenSSL writes a whole flight.
 * Bound queued bytes so blocked transports cannot grow memory indefinitely. */
#define QUEUE_LIMIT (64u * 1024u)
typedef struct packet {
    struct packet *next;
    size_t size;
    unsigned char data[];
} packet;
typedef struct {
    ewrtc_pal pal;
    packet *head, *tail;
    size_t bytes;
    long mtu;
} datagram_queue;

static void remove_head(datagram_queue *queue) {
    packet *entry = queue->head;
    queue->head = entry->next;
    if (!queue->head)
        queue->tail = NULL;
    queue->bytes -= entry->size;
    ewrtc_free(&queue->pal, entry);
}

static void clear_queue(datagram_queue *queue) {
    while (queue->head)
        remove_head(queue);
}

static int destroy_bio(BIO *bio) {
    datagram_queue *queue = BIO_get_data(bio);
    if (queue) {
        clear_queue(queue);
        ewrtc_pal pal = queue->pal;
        ewrtc_free(&pal, queue);
    }
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int read_packet(BIO *bio, char *out, int capacity) {
    datagram_queue *queue = BIO_get_data(bio);
    BIO_clear_retry_flags(bio);
    if (!out || capacity <= 0)
        return 0;
    if (!queue->head) {
        BIO_set_retry_read(bio);
        return -1;
    }
    packet *entry = queue->head;
    int size = entry->size < (size_t)capacity ? (int)entry->size : capacity;
    memcpy(out, entry->data, (size_t)size);
    /* Like UDP, a short read discards the remainder of this datagram. */
    remove_head(queue);
    return size;
}

static int write_packet(BIO *bio, const char *data, int size) {
    datagram_queue *queue = BIO_get_data(bio);
    BIO_clear_retry_flags(bio);
    if (!data || size <= 0)
        return 0;
    if ((size_t)size > QUEUE_LIMIT - queue->bytes)
        return -1;
    packet *entry = ewrtc_alloc(&queue->pal, sizeof(*entry) + (size_t)size);
    if (!entry)
        return -1;
    entry->next = NULL;
    entry->size = (size_t)size;
    memcpy(entry->data, data, entry->size);
    if (queue->tail)
        queue->tail->next = entry;
    else
        queue->head = entry;
    queue->tail = entry;
    queue->bytes += entry->size;
    return size;
}

static long control_bio(BIO *bio, int command, long value, void *ptr) {
    datagram_queue *queue = BIO_get_data(bio);
    (void)ptr;
    switch (command) {
    case BIO_CTRL_RESET:
        clear_queue(queue);
        BIO_clear_retry_flags(bio);
        return 1;
    case BIO_CTRL_EOF:
        return queue->head == NULL;
    case BIO_CTRL_PENDING:
        return queue->head ? (long)queue->head->size : 0;
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        /* The DTLS adapter drives retransmission through its tick/deadline API. */
        return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU:
    case BIO_CTRL_DGRAM_GET_MTU:
    case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
        return queue->mtu;
    case BIO_CTRL_DGRAM_SET_MTU:
        if (value <= 0)
            return 0;
        queue->mtu = value;
        return value;
    default:
        return 0;
    }
}

BIO_METHOD *ewrtc_openssl_bio_method(void) {
    BIO_METHOD *method = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(),
                                     "ewrtc datagram queue");
    if (!method)
        return NULL;
    if (!BIO_meth_set_write(method, write_packet) ||
        !BIO_meth_set_read(method, read_packet) ||
        !BIO_meth_set_ctrl(method, control_bio) ||
        !BIO_meth_set_destroy(method, destroy_bio)) {
        BIO_meth_free(method);
        return NULL;
    }
    return method;
}

BIO *ewrtc_openssl_bio_new(BIO_METHOD *method, const ewrtc_pal *pal) {
    BIO *bio = BIO_new(method);
    if (!bio)
        return NULL;
    datagram_queue *queue = ewrtc_zalloc(pal, sizeof(*queue));
    if (!queue) {
        BIO_free(bio);
        return NULL;
    }
    queue->pal = *pal;
    queue->mtu = EWRTC_MTU;
    BIO_set_data(bio, queue);
    BIO_set_init(bio, 1);
    return bio;
}
