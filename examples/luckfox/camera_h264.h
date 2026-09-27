#ifndef EWRTC_CAMERA_H264_H
#define EWRTC_CAMERA_H264_H
#include "camera_source.h"
#include <stdbool.h>
#define CAMERA_AU_CAPACITY (512 * 1024)
typedef struct {
    uint8_t au[CAMERA_AU_CAPACITY], sps[1024], pps[1024];
    size_t size, sps_size, pps_size;
    uint32_t timestamp, previous_timestamp;
    uint64_t ticks;
    uint16_t next_sequence;
    bool initialized, clock_set, broken, fragmented, idr, vcl;
    camera_frame_callback frame;
    void *user;
} camera_h264;
int camera_h264_packet(camera_h264 *, const uint8_t *, size_t);
#endif
