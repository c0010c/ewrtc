#ifndef EWRTC_TEST_OFFER_H
#define EWRTC_TEST_OFFER_H
static const char test_offer[] =
    "v=0\r\na=group:BUNDLE 0 1\r\na=ice-options:trickle\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102 "
    "103\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\na=setup:actpass\r\n"
    "a=ice-ufrag:test\r\na=ice-pwd:password\r\n"
    "a=fingerprint:sha-256 "
    "A8:D3:E5:7B:31:D6:76:9E:70:C8:03:6F:BF:EE:53:AC:BF:83:94:6C:2C:FE:0E:51:C7:F0:19:AA:8B:18:50:"
    "E9\r\n"
    "a=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF "
    "111\r\na=mid:1\r\na=sendrecv\r\na=rtcp-mux\r\na=setup:actpass\r\na=rtpmap:111 "
    "opus/48000/2\r\n";
#endif
