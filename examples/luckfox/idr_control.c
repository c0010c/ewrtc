#define _POSIX_C_SOURCE 200809L
#include "idr_control.h"
#include "idr_protocol.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

int idr_control_open(void) {
    return socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}
int idr_control_request(int fd) {
    struct sockaddr_un address = {0}; address.sun_family = AF_UNIX;
    memcpy(address.sun_path, EWRTC_IDR_SOCKET, sizeof(EWRTC_IDR_SOCKET));
    const char request[] = EWRTC_IDR_REQUEST;
    return sendto(fd, request, sizeof(request), MSG_DONTWAIT | MSG_NOSIGNAL,
                  (const struct sockaddr *)&address, sizeof(address)) == sizeof(request) ? 0 : -1;
}
