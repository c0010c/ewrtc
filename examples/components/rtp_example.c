/* Build against an installed package. No SDK Session, PAL or TLS is needed. */
#include "rtp/rtp.h"
#include <stdio.h>
int main(void) {
    const uint8_t opus[] = {0xf8, 0xff, 0xfe};
    ewrtc_rtp_packet input = {.payload_type = 111,
                              .sequence = 1,
                              .timestamp = 48000,
                              .ssrc = 42,
                              .payload = opus,
                              .payload_size = sizeof(opus)};
    uint8_t wire[1200];
    size_t size;
    if (ewrtc_rtp_write(&input, wire, sizeof(wire), &size))
        return 1;
    ewrtc_rtp_packet output;
    if (ewrtc_rtp_parse(wire, size, &output))
        return 1;
    printf("RTP: SSRC=%u sequence=%u timestamp=%u payload=%zu bytes\n", output.ssrc,
           output.sequence, output.timestamp, output.payload_size);
    return 0;
}
