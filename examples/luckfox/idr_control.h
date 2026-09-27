#ifndef EWRTC_IDR_CONTROL_H
#define EWRTC_IDR_CONTROL_H
/* Nonblocking local request: safe for SDK callbacks. Failure leaves periodic IDR fallback. */
int idr_control_open(void);
int idr_control_request(int fd);
#endif
