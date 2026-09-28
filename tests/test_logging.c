#include "pal/pal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned writes, clock_reads; char text[1024]; int level; } capture;
static void write_log(void *ctx, int level, const char *text) {
    capture *c = ctx;
    ++c->writes; c->level = level;
    assert(strlen(text) < sizeof(c->text));
    assert(!strchr(text, '\n') && !strchr(text, '\r') && !strchr(text, '\033'));
    strcpy(c->text, text);
}
static uint64_t utc(void *ctx) { ++((capture *)ctx)->clock_reads; return 1234567; }
static uint64_t mono(void *ctx) { ++((capture *)ctx)->clock_reads; return 42; }
int main(void) {
    capture c = {0};
    ewrtc_pal p = {.clock = {&c, mono, utc}, .log = {&c, write_log}};
    unsigned evaluated = 0;
    for (int level = EWRTC_LOG_DEBUG; level <= EWRTC_LOG_ERROR; ++level) {
        unsigned before = c.writes;
        bool enabled = ewrtc_log_enabled(&p, level);
        assert(enabled == (level >= EXPECT_LOG_MIN_LEVEL));
        EWRTC_LOG(&p, level, "TEST", "value=%u", ++evaluated);
        assert(c.writes == before + enabled);
        if (enabled) assert(c.level == level);
    }
    assert(evaluated == c.writes && c.clock_reads == c.writes);
    if (ewrtc_log_enabled(&p, EWRTC_LOG_ERROR)) {
        EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "%s", "peer\n\r\033[31m");
        assert(strstr(c.text, "1970-01-01T00:00:01.234Z ERROR TEST"));
        assert(strstr(c.text, "peer\\x0a\\x0d\\x1b[31m"));
        char large[2048]; memset(large, 'x', sizeof(large) - 1); large[sizeof(large) - 1] = 0;
        EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "%s", large);
        assert(strstr(c.text, " [truncated]"));
        memset(large, 1, sizeof(large) - 1);
        EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "%s", large);
        assert(strstr(c.text, " [truncated]"));
        p.clock.utc_us = NULL;
        EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "mono");
        assert(strstr(c.text, "+42ms"));
        p.clock.monotonic_ms = NULL;
        EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "no clock");
        assert(strstr(c.text, "time-unavailable"));
    }
    unsigned before = c.writes, reads = c.clock_reads, args = evaluated;
    assert(!ewrtc_log_enabled(&p, -1) && !ewrtc_log_enabled(&p, 4));
    p.log.write = NULL;
    EWRTC_LOG(&p, EWRTC_LOG_ERROR, "TEST", "%u", ++evaluated);
    assert(evaluated == args && c.writes == before && c.clock_reads == reads);
    puts("Log filtering, bounded formatting and control escaping passed");
}
