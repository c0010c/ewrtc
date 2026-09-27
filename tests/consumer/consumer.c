#if HAVE_SESSION
#include <ewrtc.h>
#endif
#if HAVE_LINUX
#include <ewrtc/platform/linux.h>
#endif

int main(void) {
#if HAVE_LINUX
    const ewrtc_pal *pal = ewrtc_pal_linux();
    if (!pal || ewrtc_pal_validate(pal, EWRTC_PAL_MEMORY | EWRTC_PAL_THREADS | EWRTC_PAL_NETWORK))
        return 1;
    ewrtc_address address;
    if (ewrtc_address_parse("127.0.0.1", 1234, &address))
        return 2;
#endif
#if HAVE_SESSION
    ewrtc_context_config runtime;
    ewrtc_context_config_init(&runtime);
    ewrtc_session_config config;
    ewrtc_session_config_init(&config);
#if HAVE_LINUX
    runtime.pal = *pal;
    ewrtc_context *context = NULL;
    if (ewrtc_context_create(&runtime, &context))
        return 3;
    ewrtc_callbacks callbacks = {0};
    ewrtc_session *session = NULL;
    if (ewrtc_session_create(context, &config, &callbacks, NULL, &session))
        return 4;
    ewrtc_stats stats;
    if (ewrtc_session_get_stats(session, &stats))
        return 5;
    if (ewrtc_session_destroy(session) || ewrtc_context_destroy(context))
        return 6;
#else
    /* Custom-PAL packages still link all session operations. */
    ewrtc_context *context = NULL;
    if (ewrtc_context_create(NULL, &context) != EWRTC_INVALID)
        return 7;
    if (ewrtc_session_destroy(NULL) != EWRTC_INVALID)
        return 8;
#endif
#endif
    return 0;
}
