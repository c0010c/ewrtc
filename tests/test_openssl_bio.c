#include "dtls/openssl_bio.h"
#include <openssl/crypto.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t live;
    bool fail;
} memory;
static void *allocate(void *ctx, size_t size) {
    memory *m = ctx;
    if (m->fail)
        return NULL;
    void *p = malloc(size);
    if (p)
        ++m->live;
    return p;
}
static void release(void *ctx, void *p) {
    memory *m = ctx;
    if (p) {
        assert(m->live);
        --m->live;
        free(p);
    }
}

int main(void) {
    /* Catch mixed headers/libraries as well as a silently selected system TLS. */
    assert(OPENSSL_VERSION_NUMBER == 0x1010117fL);
    assert(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
    memory m = {0};
    ewrtc_pal pal = {.memory = {&m, allocate, NULL, release}};
    BIO_METHOD *method = ewrtc_openssl_bio_method();
    assert(method);
    m.fail = 1;
    assert(!ewrtc_openssl_bio_new(method, &pal) && !m.live);
    m.fail = 0;
    BIO *bio = ewrtc_openssl_bio_new(method, &pal);
    assert(bio);
    char out[32];
    assert(BIO_read(bio, out, sizeof(out)) == -1 && BIO_should_read(bio));
    /* Separate writes must never be merged, even with a large read buffer. */
    assert(BIO_write(bio, "first", 5) == 5);
    assert(!BIO_should_retry(bio));
    assert(BIO_write(bio, "second", 6) == 6);
    assert(BIO_ctrl_pending(bio) == 5);
    assert(BIO_read(bio, out, sizeof(out)) == 5 && !memcmp(out, "first", 5));
    assert(BIO_ctrl_pending(bio) == 6);
    /* A truncated datagram must not leave a tail that looks like a new packet. */
    assert(BIO_read(bio, out, 3) == 3 && !memcmp(out, "sec", 3));
    assert(!BIO_ctrl_pending(bio));
    m.fail = 1;
    assert(BIO_write(bio, "fail", 4) == -1 && !BIO_should_retry(bio));
    m.fail = 0;
    assert(!BIO_ctrl_pending(bio));
    /* A full queue rejects a write atomically, keeping prior packets intact. */
    char packet[1024] = {0};
    for (int i = 0; i < 64; ++i)
        assert(BIO_write(bio, packet, sizeof(packet)) == sizeof(packet));
    assert(BIO_write(bio, "x", 1) == -1);
    assert(BIO_ctrl_pending(bio) == sizeof(packet));
    assert(BIO_reset(bio) == 1 && BIO_eof(bio));
    assert(m.live == 1);
    assert(BIO_write(bio, "queued", 6) == 6);
    BIO_free(bio);
    BIO_meth_free(method);
    assert(!m.live);
    return 0;
}
