#include "sdp/sdp.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int append(char *out, size_t cap, size_t *used, const char *fmt, ...) {
    if (*used >= cap)
        return EWRTC_INVALID;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used)
        return EWRTC_INVALID;
    *used += (size_t)n;
    return 0;
}

static void copy_value(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len >= cap) {
        dst[0] = 0;
        return;
    }
    memcpy(dst, src, len + 1);
}
static bool valid_fingerprint(const char *text) {
    if (strlen(text) != 95)
        return false;
    for (int i = 0; i < 95; ++i)
        if ((i + 1) % 3 == 0 ? text[i] != ':' : !isxdigit((unsigned char)text[i]))
            return false;
    return true;
}
static bool mode_one(const char *line) {
    const char *at = strstr(line, "packetization-mode=");
    if (!at)
        return false;
    at += strlen("packetization-mode=");
    return at[0] == '1' && (at[1] == ';' || at[1] == 0);
}

/* The SDP parser accepts the Chrome subset used by this SDK and rejects
 * unsupported media instead of silently advertising an unusable track. */
static int parse_description(const ewrtc_pal *pal, const char *text, ewrtc_sdp_offer *o, bool answer) {
    if (ewrtc_pal_validate(pal, EWRTC_PAL_ALLOCATOR) || !text || !o || strlen(text) > EWRTC_MAX_SDP)
        return EWRTC_INVALID;
    memset(o, 0, sizeof(*o));
    o->video_pt = o->audio_pt = o->rtx_pt = -1;
    char *copy = ewrtc_strdup(pal, text);
    if (!copy)
        return EWRTC_NOMEM;
    int section = 0, sections = 0;
    bool audio_mux = false, video_mux = false;
    bool invalid = false;
    int setup[3] = {-1, -1, -1};
    ewrtc_direction session_direction = EWRTC_SENDRECV;
    bool listed[3][128] = {{0}}, nack[128] = {0}, pli[128] = {0};
    char bundle_mids[128] = {0};
    int opus_candidate = -1;
    bool h264[128] = {0}, rtx[128] = {0}, mode1[128] = {0};
    int rtx_apt[128];
    char profiles[128][7] = {{0}};
    for (int i = 0; i < 128; ++i)
        rtx_apt[i] = -1;
    for (char *line = copy, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = 0;
        if (!strncmp(line, "m=", 2)) {
            char media[32] = {0}, protocol[32] = {0};
            unsigned port = 0;
            if (sscanf(line + 2, "%31s %u %31s", media, &port, protocol) != 3 || !port ||
                port > 65535 || strcmp(protocol, "UDP/TLS/RTP/SAVPF"))
                invalid = true;
            if (!strncmp(line, "m=audio ", 8)) {
                if (o->audio)
                    invalid = true;
                section = 1;
                o->audio = true;
                if (!sections)
                    o->audio_first = true;
            } else if (!strncmp(line, "m=video ", 8)) {
                if (o->video)
                    invalid = true;
                section = 2;
                o->video = true;
            } else {
                section = 3;
                invalid = true;
            }
            if (section < 3) {
                int consumed = 0;
                sscanf(line + 2, "%*s %*u %*s %n", &consumed);
                const char *at = line + 2 + consumed;
                while (*at) {
                    char *end;
                    long pt = strtol(at, &end, 10);
                    if (end == at || pt < 0 || pt > 127) { invalid = true; break; }
                    listed[section][pt] = true;
                    at = end;
                    while (*at == ' ') ++at;
                }
                if (section == 1) o->audio_direction = session_direction;
                else o->video_direction = session_direction;
            }
            sections++;
        } else if (!strncmp(line, "a=group:BUNDLE ", 15)) {
            o->bundle = true;
            copy_value(bundle_mids, sizeof(bundle_mids), line + 15);
        } else if (!strcmp(line, "a=rtcp-mux") && (section == 1 || section == 2)) {
            if (section == 1)
                audio_mux = true;
            if (section == 2)
                video_mux = true;
        } else if (!strcmp(line, "a=sendrecv") || !strcmp(line, "a=sendonly") ||
                   !strcmp(line, "a=recvonly") || !strcmp(line, "a=inactive")) {
            ewrtc_direction dir = !strcmp(line + 2, "sendonly") ? EWRTC_SENDONLY :
                !strcmp(line + 2, "recvonly") ? EWRTC_RECVONLY :
                !strcmp(line + 2, "inactive") ? EWRTC_INACTIVE : EWRTC_SENDRECV;
            if (section == 0) session_direction = dir;
            if (section == 1) o->audio_direction = dir;
            if (section == 2) o->video_direction = dir;
        } else if (!strncmp(line, "a=setup:", 8) && section < 3) {
            int role = !strcmp(line + 8, "actpass") ? EWRTC_SETUP_ACTPASS :
                !strcmp(line + 8, "active") ? EWRTC_SETUP_ACTIVE :
                !strcmp(line + 8, "passive") ? EWRTC_SETUP_PASSIVE : -1;
            if (role < 0 || (setup[section] >= 0 && setup[section] != role)) invalid = true;
            setup[section] = role;
        } else if (!strncmp(line, "a=ice-options:", 14) && strstr(line, "trickle")) {
            o->trickle = true;
        } else if (!strncmp(line, "a=ice-ufrag:", 12)) {
            if (o->ice_ufrag[0] && strcmp(o->ice_ufrag, line + 12)) invalid = true;
            copy_value(o->ice_ufrag, sizeof(o->ice_ufrag), line + 12);
        } else if (!strncmp(line, "a=ice-pwd:", 10)) {
            if (o->ice_pwd[0] && strcmp(o->ice_pwd, line + 10)) invalid = true;
            copy_value(o->ice_pwd, sizeof(o->ice_pwd), line + 10);
        } else if (!strncmp(line, "a=fingerprint:sha-256 ", 22)) {
            if (o->fingerprint[0] && ewrtc_ascii_casecmp(o->fingerprint, line + 22)) invalid = true;
            copy_value(o->fingerprint, sizeof(o->fingerprint), line + 22);
        } else if (!strncmp(line, "a=mid:", 6)) {
            if (section == 1)
                copy_value(o->audio_mid, sizeof(o->audio_mid), line + 6);
            if (section == 2)
                copy_value(o->video_mid, sizeof(o->video_mid), line + 6);
        } else if (!strncmp(line, "a=rtpmap:", 9)) {
            int pt, clock = 0, channels = 0;
            char codec[32] = {0};
            int count = sscanf(line + 9, "%d %31[^/]/%d/%d", &pt, codec, &clock, &channels);
            if (count >= 3 && pt >= 0 && pt < 128) {
                if (section == 1 && !ewrtc_ascii_casecmp(codec, "opus") && clock == 48000 &&
                    count == 4 && channels == 2)
                    opus_candidate = pt;
                if (section == 2 && !ewrtc_ascii_casecmp(codec, "H264") && clock == 90000)
                    h264[pt] = true;
                if (section == 2 && !ewrtc_ascii_casecmp(codec, "rtx") && clock == 90000)
                    rtx[pt] = true;
            }
        } else if (section == 2 && !strncmp(line, "a=ssrc-group:FID ", 17)) {
            if (sscanf(line + 17, "%u %u", &o->video_ssrc, &o->rtx_ssrc) != 2) invalid = true;
        } else if (section == 2 && !strncmp(line, "a=ssrc:", 7) && !o->video_ssrc) {
            sscanf(line + 7, "%u", &o->video_ssrc);
        } else if (section == 2 && !strncmp(line, "a=rtcp-fb:", 10)) {
            int pt; char feedback[32] = {0};
            if (sscanf(line + 10, "%d %31[^\r\n]", &pt, feedback) == 2 && pt >= 0 && pt < 128) {
                if (!strcmp(feedback, "nack")) nack[pt] = true;
                if (!strcmp(feedback, "nack pli")) pli[pt] = true;
            }
        } else if (section == 2 && !strncmp(line, "a=fmtp:", 7)) {
            int pt;
            if (sscanf(line + 7, "%d", &pt) != 1 || pt < 0 || pt >= 128)
                continue;
            mode1[pt] = mode_one(line);
            const char *profile = strstr(line, "profile-level-id=");
            if (profile && strlen(profile + 17) >= 6) {
                memcpy(profiles[pt], profile + 17, 6);
                profiles[pt][6] = 0;
            }
            const char *apt = strstr(line, "apt=");
            if (apt)
                rtx_apt[pt] = atoi(apt + 4);
        }
    }
    ewrtc_free(pal, copy);
    char first_mid[32], second_mid[32], third_mid[32];
    int bundle_count = sscanf(bundle_mids, "%31s %31s %31s", first_mid, second_mid, third_mid);
    bool mids_match = bundle_count == 2 &&
                      ((!strcmp(first_mid, o->video_mid) && !strcmp(second_mid, o->audio_mid)) ||
                       (!strcmp(first_mid, o->audio_mid) && !strcmp(second_mid, o->video_mid)));
    for (int pass = 0; pass < 2 && o->video_pt < 0; ++pass)
        for (int pt = 0; pt < 128; ++pt)
            if (listed[2][pt] && h264[pt] && mode1[pt] &&
                ((pass == 0 && !ewrtc_ascii_ncasecmp(profiles[pt], "42e0", 4)) ||
                 (pass == 1 && !ewrtc_ascii_ncasecmp(profiles[pt], "4200", 4)))) {
                o->video_pt = pt;
                copy_value(o->h264_profile, sizeof(o->h264_profile), profiles[pt]);
                break;
            }
    for (int pt = 0; pt < 128; ++pt)
        if (listed[2][pt] && rtx[pt] && rtx_apt[pt] == o->video_pt) {
            o->rtx_pt = pt;
            break;
        }
    o->rtcp_mux = audio_mux && video_mux;
    int audio_setup = setup[1] >= 0 ? setup[1] : setup[0];
    int video_setup = setup[2] >= 0 ? setup[2] : setup[0];
    if (audio_setup < 0 || audio_setup != video_setup || (answer && audio_setup == EWRTC_SETUP_ACTPASS))
        invalid = true;
    o->setup = (ewrtc_setup)audio_setup;
    if (invalid || sections != 2 || !o->video || !o->audio || !o->bundle || !mids_match ||
        !o->rtcp_mux || !o->ice_ufrag[0] || !o->ice_pwd[0] || !o->fingerprint[0] ||
        !valid_fingerprint(o->fingerprint) || !o->audio_mid[0] || !o->video_mid[0] ||
        !strcmp(o->audio_mid, o->video_mid) || o->video_pt < 0 || opus_candidate < 0 ||
        !listed[1][opus_candidate] || o->video_pt == opus_candidate || o->rtx_pt == opus_candidate ||
        o->rtx_pt == o->video_pt)
        return EWRTC_INVALID;
    for (unsigned i = 0; i < 6; ++i)
        if (!isxdigit((unsigned char)o->h264_profile[i])) return EWRTC_INVALID;
    o->audio_pt = opus_candidate;
    o->nack = nack[o->video_pt];
    o->pli = pli[o->video_pt];
    return 0;
}


int ewrtc_sdp_parse_offer(const ewrtc_pal *p, const char *s, ewrtc_sdp_offer *o) {
    return parse_description(p, s, o, false);
}
int ewrtc_sdp_parse_answer(const ewrtc_pal *p, const char *s, ewrtc_sdp_offer *o) {
    return parse_description(p, s, o, true);
}
static ewrtc_direction reverse(ewrtc_direction d) {
    return d == EWRTC_SENDONLY ? EWRTC_RECVONLY : d == EWRTC_RECVONLY ? EWRTC_SENDONLY : d;
}
static bool valid_answer_direction(ewrtc_direction offer, ewrtc_direction answer) {
    return (!ewrtc_direction_sends(answer) || ewrtc_direction_receives(offer)) &&
           (!ewrtc_direction_receives(answer) || ewrtc_direction_sends(offer));
}
int ewrtc_sdp_validate_answer(const ewrtc_sdp_offer *o, const ewrtc_sdp_offer *a) {
    if (!o || !a || a->setup == EWRTC_SETUP_ACTPASS ||
        (o->setup == EWRTC_SETUP_ACTIVE && a->setup != EWRTC_SETUP_PASSIVE) ||
        (o->setup == EWRTC_SETUP_PASSIVE && a->setup != EWRTC_SETUP_ACTIVE) ||
        o->video_pt != a->video_pt || o->audio_pt != a->audio_pt ||
        (a->rtx_pt >= 0 && o->rtx_pt != a->rtx_pt) ||
        strcmp(o->video_mid, a->video_mid) || strcmp(o->audio_mid, a->audio_mid) ||
        o->audio_first != a->audio_first ||
        ewrtc_ascii_ncasecmp(o->h264_profile, a->h264_profile, 4) ||
        !valid_answer_direction(o->video_direction, a->video_direction) ||
        !valid_answer_direction(o->audio_direction, a->audio_direction) ||
        (a->nack && !o->nack) || (a->pli && !o->pli))
        return EWRTC_INVALID;
    return 0;
}
static const char *direction(ewrtc_direction d) {
    static const char *names[] = {"sendrecv", "sendonly", "recvonly", "inactive"};
    return names[d];
}
static const char *setup_name(ewrtc_setup setup) {
    return setup == EWRTC_SETUP_ACTPASS ? "actpass" : setup == EWRTC_SETUP_ACTIVE ? "active" : "passive";
}
static int add_media(char *out, size_t cap, size_t *used, const ewrtc_sdp_offer *o,
                     const ewrtc_sdp_local *local, bool video, uint32_t ssrc, uint32_t rtx_ssrc) {
    int pt = video ? o->video_pt : o->audio_pt;
    const char *kind = video ? "video" : "audio";
    ewrtc_direction dir = video ? o->video_direction : o->audio_direction;
    if (append(out, cap, used, "m=%s 9 UDP/TLS/RTP/SAVPF %d", kind, pt)) return EWRTC_INVALID;
    if (video && o->rtx_pt >= 0 && append(out, cap, used, " %d", o->rtx_pt)) return EWRTC_INVALID;
    if (append(out, cap, used,
               "\r\nc=IN IP4 0.0.0.0\r\na=mid:%s\r\na=%s\r\na=rtcp-mux\r\n"
               "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:sha-256 %s\r\na=setup:%s\r\n",
               video ? o->video_mid : o->audio_mid, direction(dir), local->ufrag, local->pwd,
               local->fingerprint, setup_name(o->setup))) return EWRTC_INVALID;
    if (video) {
        if (append(out, cap, used, "a=rtpmap:%d H264/90000\r\n"
                   "a=fmtp:%d profile-level-id=%s;packetization-mode=1;level-asymmetry-allowed=1\r\n",
                   pt, pt, o->h264_profile)) return EWRTC_INVALID;
        if (o->nack && append(out, cap, used, "a=rtcp-fb:%d nack\r\n", pt)) return EWRTC_INVALID;
        if (o->pli && append(out, cap, used, "a=rtcp-fb:%d nack pli\r\n", pt)) return EWRTC_INVALID;
        if (o->rtx_pt >= 0 && append(out, cap, used, "a=rtpmap:%d rtx/90000\r\na=fmtp:%d apt=%d\r\n",
                                    o->rtx_pt, o->rtx_pt, pt)) return EWRTC_INVALID;
    } else if (append(out, cap, used, "a=rtpmap:%d opus/48000/2\r\na=fmtp:%d minptime=10;useinbandfec=1\r\n", pt, pt))
        return EWRTC_INVALID;
    if (ewrtc_direction_sends(dir)) {
        if (append(out, cap, used, "a=msid:ewrtc %s\r\na=ssrc:%u cname:ewrtc\r\na=ssrc:%u msid:ewrtc %s\r\n",
                   kind, ssrc, ssrc, kind)) return EWRTC_INVALID;
        if (video && o->rtx_pt >= 0 && append(out, cap, used,
            "a=ssrc-group:FID %u %u\r\na=ssrc:%u cname:ewrtc\r\na=ssrc:%u msid:ewrtc video\r\n",
            ssrc, rtx_ssrc, rtx_ssrc, rtx_ssrc)) return EWRTC_INVALID;
    }
    return 0;
}
int ewrtc_sdp_make_description(const ewrtc_sdp_offer *o, const ewrtc_sdp_local *local,
                               uint32_t video_ssrc, uint32_t rtx_ssrc, uint32_t audio_ssrc,
                               char *out, size_t cap) {
    if (!o || !local || !local->ufrag || !local->pwd || !local->fingerprint || !out || !cap ||
        (unsigned)o->video_direction > EWRTC_INACTIVE || (unsigned)o->audio_direction > EWRTC_INACTIVE ||
        (unsigned)o->setup > EWRTC_SETUP_PASSIVE || o->video_pt < 0 || o->video_pt > 127 ||
        o->audio_pt < 0 || o->audio_pt > 127 || o->rtx_pt < -1 || o->rtx_pt > 127)
        return EWRTC_INVALID;
    size_t used = 0;
    if (append(out, cap, &used,
               "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
               "a=group:BUNDLE %s %s\r\na=msid-semantic: WMS ewrtc\r\na=ice-options:trickle\r\n",
               o->audio_first ? o->audio_mid : o->video_mid,
               o->audio_first ? o->video_mid : o->audio_mid)) return EWRTC_INVALID;
    for (int i = 0; i < 2; ++i) {
        bool video = o->audio_first ? i == 1 : i == 0;
        if (add_media(out, cap, &used, o, local, video, video ? video_ssrc : audio_ssrc, rtx_ssrc))
            return EWRTC_INVALID;
    }
    return 0;
}
int ewrtc_sdp_make_answer(const ewrtc_sdp_offer *offer, const ewrtc_sdp_local *local,
                          uint32_t video_ssrc, uint32_t rtx_ssrc, uint32_t audio_ssrc,
                          char *out, size_t cap) {
    if (!offer) return EWRTC_INVALID;
    ewrtc_sdp_offer answer = *offer;
    answer.video_direction = reverse(offer->video_direction);
    answer.audio_direction = reverse(offer->audio_direction);
    answer.setup = offer->setup == EWRTC_SETUP_PASSIVE ? EWRTC_SETUP_ACTIVE : EWRTC_SETUP_PASSIVE;
    return ewrtc_sdp_make_description(&answer, local, video_ssrc, rtx_ssrc, audio_ssrc, out, cap);
}
