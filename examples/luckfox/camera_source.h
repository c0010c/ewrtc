#ifndef EWRTC_CAMERA_SOURCE_H
#define EWRTC_CAMERA_SOURCE_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
typedef void (*camera_frame_callback)(const uint8_t *, size_t, uint64_t, int, void *);
/* Local firmware RTSP only: TCP interleaving, H.264 single NAL/STAP-A/FU-A.
 * Returns on stop or source failure. Buffers are borrowed during callback. */
int camera_source_run(const atomic_bool *stop, camera_frame_callback frame, void *user);
#endif
