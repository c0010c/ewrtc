#ifndef EWRTC_RTP_H
#define EWRTC_RTP_H
#include "common/common.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint8_t payload_type;
    bool marker;
    uint16_t sequence;
    uint32_t timestamp, ssrc;
    const uint8_t *payload;
    size_t payload_size;
} ewrtc_rtp_packet;
/* Parsed payload borrows the input packet. Writer emits the base RTP header. */
int ewrtc_rtp_parse(const uint8_t *, size_t, ewrtc_rtp_packet *);
int ewrtc_rtp_write(const ewrtc_rtp_packet *, uint8_t *, size_t, size_t *);
typedef struct {
    uint8_t type, count;
    const uint8_t *bytes;
    size_t size;
} ewrtc_rtcp_packet;
/* Walk compound RTCP using offset initialized to zero. End returns EWRTC_AGAIN. */
int ewrtc_rtcp_next(const uint8_t *, size_t, size_t *offset, ewrtc_rtcp_packet *);
int ewrtc_rtcp_write_sr(uint32_t ssrc, uint32_t timestamp, uint32_t packets, uint32_t octets,
                        uint64_t utc_us, uint8_t *, size_t);
int ewrtc_rtcp_write_feedback(uint8_t type, uint8_t fmt, uint32_t sender, uint32_t media,
                              const uint8_t *fci, size_t fci_size, uint8_t *, size_t, size_t *);

#ifdef __cplusplus
}
#endif
#endif
