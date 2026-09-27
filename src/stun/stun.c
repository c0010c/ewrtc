#include "stun/stun.h"
#include <stdio.h>
#include <string.h>
static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}
int ewrtc_stun_random_transaction(const ewrtc_pal *pal, uint8_t out[12]) {
    return !pal || !pal->random.bytes || !out ? EWRTC_INVALID : ewrtc_random_bytes(pal, out, 12);
}
int ewrtc_stun_begin(ewrtc_stun_writer *w, uint16_t type, const uint8_t transaction[12]) {
    if (!w || !transaction)
        return EWRTC_INVALID;
    memset(w, 0, sizeof(*w));
    w16(w->bytes, type);
    w32(w->bytes + 4, STUN_COOKIE);
    memcpy(w->bytes + 8, transaction, 12);
    w->len = 20;
    return 0;
}
int ewrtc_stun_add(ewrtc_stun_writer *w, uint16_t type, const void *data, size_t size) {
    if (!w || (size && !data) || w->len < 20 || w->len > sizeof(w->bytes) || size > UINT16_MAX ||
        w->len + 4 + ((size + 3) & ~3u) > sizeof(w->bytes))
        return EWRTC_INVALID;
    w16(w->bytes + w->len, type);
    w16(w->bytes + w->len + 2, (uint16_t)size);
    if (size)
        memcpy(w->bytes + w->len + 4, data, size);
    w->len += 4 + ((size + 3) & ~3u);
    w16(w->bytes + 2, (uint16_t)(w->len - 20));
    return 0;
}
int ewrtc_stun_add_xor_address(ewrtc_stun_writer *w, uint16_t type, const ewrtc_address *address) {
    if (!address)
        return EWRTC_INVALID;
    uint8_t value[8] = {0, 1};
    uint16_t port = address->port ^ (uint16_t)(STUN_COOKIE >> 16);
    uint32_t ip = address->ipv4 ^ STUN_COOKIE;
    w16(value + 2, port);
    w32(value + 4, ip);
    return ewrtc_stun_add(w, type, value, sizeof(value));
}
int ewrtc_stun_add_integrity(ewrtc_stun_writer *w, const uint8_t *key, size_t key_size,
                             ewrtc_crypto_backend backend) {
    if (!w || !key || w->len < 20 || w->len > sizeof(w->bytes) - 24)
        return EWRTC_INVALID;
    w16(w->bytes + 2, (uint16_t)(w->len + 24 - 20));
    uint8_t digest[20];
    int result = ewrtc_crypto_hmac_sha1(backend, w->bytes, w->len, key, key_size, digest);
    if (result)
        return result;
    return ewrtc_stun_add(w, ATTR_MESSAGE_INTEGRITY, digest, sizeof(digest));
}
int ewrtc_stun_add_fingerprint(ewrtc_stun_writer *w) {
    if (!w || w->len < 20 || w->len > sizeof(w->bytes) - 8)
        return EWRTC_INVALID;
    w16(w->bytes + 2, (uint16_t)(w->len + 8 - 20));
    uint32_t value = crc32(w->bytes, w->len) ^ 0x5354554eu;
    uint8_t bytes[4];
    w32(bytes, value);
    return ewrtc_stun_add(w, ATTR_FINGERPRINT, bytes, sizeof(bytes));
}
int ewrtc_stun_parse(const uint8_t *data, size_t size, ewrtc_stun_packet *packet) {
    if (!data || !packet || size < 20 || size > 2048 || (data[0] & 0xc0) ||
        u32(data + 4) != STUN_COOKIE || u16(data + 2) + 20u != size)
        return EWRTC_INVALID;
    memset(packet, 0, sizeof(*packet));
    packet->bytes = data;
    packet->len = size;
    packet->type = u16(data);
    memcpy(packet->transaction, data + 8, 12);
    for (size_t at = 20; at < size;) {
        if (at + 4 > size || packet->count == 48)
            return EWRTC_INVALID;
        uint16_t len = u16(data + at + 2);
        size_t next = at + 4 + ((len + 3u) & ~3u);
        if (next > size)
            return EWRTC_INVALID;
        ewrtc_stun_attr *attr = &packet->attrs[packet->count++];
        attr->type = u16(data + at);
        attr->length = len;
        attr->value = data + at + 4;
        attr->offset = at;
        at = next;
    }
    const ewrtc_stun_attr *fp = ewrtc_stun_find(packet, ATTR_FINGERPRINT);
    if (fp && (fp->length != 4 || fp->offset + 8 != size ||
               u32(fp->value) != (crc32(data, fp->offset) ^ 0x5354554eu)))
        return EWRTC_INVALID;
    return 0;
}
const ewrtc_stun_attr *ewrtc_stun_find(const ewrtc_stun_packet *packet, uint16_t type) {
    if (!packet || packet->count > 48)
        return NULL;
    for (size_t i = 0; i < packet->count; ++i)
        if (packet->attrs[i].type == type)
            return &packet->attrs[i];
    return NULL;
}
int ewrtc_stun_verify_integrity(const ewrtc_stun_packet *packet, const uint8_t *key,
                                size_t key_size, ewrtc_crypto_backend backend) {
    if (!packet || !packet->bytes || !key || packet->len > 2048)
        return EWRTC_INVALID;
    const ewrtc_stun_attr *attr = ewrtc_stun_find(packet, ATTR_MESSAGE_INTEGRITY);
    if (!attr || attr->length != 20 || attr->offset > packet->len ||
        packet->len - attr->offset < 24)
        return EWRTC_INVALID;
    uint8_t copy[2048];
    memcpy(copy, packet->bytes, attr->offset);
    w16(copy + 2, (uint16_t)(attr->offset + 24 - 20));
    uint8_t actual[20];
    int result = ewrtc_crypto_hmac_sha1(backend, copy, attr->offset, key, key_size, actual);
    if (result)
        return result;
    unsigned diff = 0;
    for (int i = 0; i < 20; ++i)
        diff |= actual[i] ^ attr->value[i];
    return diff ? EWRTC_SECURITY : EWRTC_OK;
}
int ewrtc_stun_decode_xor_address(const ewrtc_stun_packet *packet, const ewrtc_stun_attr *attr,
                                  ewrtc_address *address) {
    (void)packet;
    if (!address || !attr || !attr->value || attr->length != 8 || attr->value[1] != 1)
        return EWRTC_INVALID;
    memset(address, 0, sizeof(*address));
    address->port = u16(attr->value + 2) ^ (uint16_t)(STUN_COOKIE >> 16);
    address->ipv4 = u32(attr->value + 4) ^ STUN_COOKIE;
    return 0;
}
int ewrtc_stun_long_term_key(const ewrtc_pal *pal, const char *user, const char *realm,
                             const char *password, ewrtc_crypto_backend backend, uint8_t out[16]) {
    if (ewrtc_pal_validate(pal, EWRTC_PAL_ALLOCATOR) || !user || !realm || !password || !out)
        return EWRTC_INVALID;
    size_t n = strlen(user) + strlen(realm) + strlen(password) + 3;
    char *text = ewrtc_alloc(pal, n);
    if (!text)
        return EWRTC_NOMEM;
    snprintf(text, n, "%s:%s:%s", user, realm, password);
    int result = ewrtc_crypto_md5(backend, text, n - 1, out);
    ewrtc_free(pal, text);
    return result;
}
