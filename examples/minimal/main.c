#include <ewrtc.h>
#include <ewrtc/platform/linux.h>
#include <stdio.h>

int main(void) {
    ewrtc_context_config runtime;
    ewrtc_context_config_init(&runtime);
    runtime.pal = *ewrtc_pal_linux();
    ewrtc_context *context = NULL;
    ewrtc_result result = ewrtc_context_create(&runtime, &context);
    if (result != EWRTC_OK) {
        fprintf(stderr, "context_create: %d\n", (int)result);
        return 1;
    }

    ewrtc_session_config config;
    ewrtc_session_config_init(&config);
    ewrtc_callbacks callbacks = {0};
    ewrtc_session *session = NULL;
    result = ewrtc_session_create(context, &config, &callbacks, NULL, &session);
    if (result != EWRTC_OK) {
        fprintf(stderr, "session_create: %d\n", (int)result);
        ewrtc_context_destroy(context);
        return 1;
    }
    puts("ewrtc context and session created successfully.");
    /* Add application signaling and media callbacks to establish a connection.
       Always destroy sessions before their context, outside SDK callbacks. */
    result = ewrtc_session_destroy(session);
    if (result != EWRTC_OK) {
        fprintf(stderr, "session_destroy: %d\n", (int)result);
        return 1;
    }
    result = ewrtc_context_destroy(context);
    return result == EWRTC_OK ? 0 : 1;
}
