#ifndef EWRTC_MUX_IO_H
#define EWRTC_MUX_IO_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
typedef struct mux_io mux_io;
typedef struct { uint32_t id; char kind[32]; size_t length; char *data; } mux_record;
/* Wire: <session-id> <TYPE> <byte-length>\n<payload>. ID zero is process-wide. */
mux_io *mux_io_create(const volatile sig_atomic_t *stop);
int mux_io_read(mux_io *, mux_record *);
/* Bounded queue: callbacks never wait for pipe or network writes. */
void mux_io_send(mux_io *, uint32_t, const char *, const char *);
void mux_io_destroy(mux_io *);
#endif
