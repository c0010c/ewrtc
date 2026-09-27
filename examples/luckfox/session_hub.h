#ifndef EWRTC_SESSION_HUB_H
#define EWRTC_SESSION_HUB_H
#include "mux_io.h"
#include "camera_source.h"
typedef struct session_hub session_hub;
session_hub *session_hub_create(mux_io *, unsigned max_sessions);
void session_hub_command(session_hub *, const mux_record *);
void session_hub_frame(const uint8_t *, size_t, uint64_t, int, void *);
void session_hub_source_error(session_hub *);
void session_hub_destroy(session_hub *);
#endif
