#ifndef EWRTC_ICE_PRIVATE_H
#define EWRTC_ICE_PRIVATE_H
#include "ice/ice.h"
typedef ewrtc_ice ice_adapter;
typedef struct {
    int (*start)(ice_adapter *);
    int (*tick)(ice_adapter *);
    int (*remote_credentials)(ice_adapter *, const char *, const char *);
    int (*add_candidate)(ice_adapter *, const char *);
    int (*end_candidates)(ice_adapter *);
    int (*send)(ice_adapter *, const uint8_t *, size_t);
    int (*selected)(ice_adapter *, char *, size_t, char *, size_t);
    void (*destroy)(ice_adapter *);
    ewrtc_socket (*get_socket)(ice_adapter *);
    int (*drain)(ice_adapter *, unsigned *, unsigned *, bool *);
    int (*timers)(ice_adapter *);
    uint64_t (*deadline)(ice_adapter *);
} ice_ops;

struct ewrtc_ice {
    const ice_ops *ops;
    ewrtc_ice_events events;
    ewrtc_pal pal;
    bool started;
    ewrtc_ice_delivery delivery;
    char ufrag[256], pwd[256];
};

ice_adapter *ice_juice_create(const ewrtc_ice_config *, const ewrtc_ice_events *, int *);
ice_adapter *ice_native_create(const ewrtc_ice_config *, const ewrtc_ice_events *, int *);

#endif
