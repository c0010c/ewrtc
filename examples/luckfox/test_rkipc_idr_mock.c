#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <unistd.h>
/* Export with -rdynamic to test the bridge without camera hardware. */
int RK_MPI_VENC_RequestIDR(int channel, int instant) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    printf("%d %d %llu\n", channel, instant,
           (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000);
    fflush(stdout);
    return 0;
}
int main(void) {
    char data;
    while (read(STDIN_FILENO, &data, 1) > 0) {}
    return 0;
}
