# 视频接收与主动协商

设备 SDK 仍是纯 C，不链接 Google libwebrtc。此次补充 `on_video`、主动生成 offer 和接收 answer，并保留浏览器发 offer、设备回答 answer 的原有流程。

## 业务接入

先创建共享 context 并在 context 配置中注入 PAL，再用 `ewrtc_session_config_init` 初始化每路配置。音频和视频分别支持 `EWRTC_SENDRECV`（默认）、`EWRTC_SENDONLY`、`EWRTC_RECVONLY`、`EWRTC_INACTIVE`。

```c
#include <ewrtc.h>
#include <ewrtc/platform/linux.h>

/* 这些回调由业务实现。 */
static void local_sdp(ewrtc_session *, const char *, void *);
static void local_candidate(ewrtc_session *, const char *, void *);
static void gathering_done(ewrtc_session *, void *);
static void video(ewrtc_session *, const uint8_t *, size_t, uint32_t, int, void *);
static void error(ewrtc_session *, ewrtc_result, const char *, void *);

int start_receiver(ewrtc_context *context, ewrtc_session **out, void *user) {
    ewrtc_session_config config;
    ewrtc_session_config_init(&config);
    config.video_direction = EWRTC_RECVONLY;
    config.audio_direction = EWRTC_SENDRECV;
    ewrtc_callbacks callbacks = {
        .on_local_sdp = local_sdp,
        .on_local_candidate = local_candidate,
        .on_gathering_done = gathering_done,
        .on_video = video,
        .on_error = error,
    };
    ewrtc_result r = ewrtc_session_create(context, &config, &callbacks, user, out);
    if (r != EWRTC_OK) return r;
    r = ewrtc_session_create_offer(*out);
    if (r != EWRTC_OK) {
        ewrtc_session_destroy(*out);
        *out = NULL;
    }
    return r;
}
```

主动协商顺序：

1. 创建 Session，调用 `ewrtc_session_create_offer(session)`。
2. `on_local_sdp` 在所属 context worker交付本地 **offer**；业务按 offer 类型通过自己的信令通道传给对端。
3. 对端返回 answer 后调用 `ewrtc_session_set_remote_answer(session, answer)`。必须在本地 offer 回调开始后提交。
4. 通过 `on_local_candidate`、`on_gathering_done` 发送增量候选；对端候选传给 `ewrtc_session_add_remote_candidate`，结束消息传给 `ewrtc_session_end_remote_candidates`。
5. 等待 `EWRTC_CONNECTED`；`on_video` 接收完整视频 AU，原有 `on_audio` 接收 Opus 包。

被动协商仍调用 `ewrtc_session_set_remote_offer(session, offer)`，此时同一个 `on_local_sdp` 回调交付的是 **answer**。应用知道自己选择的协商路径，负责为信令标注 offer/answer 类型。两个入口不能在同一 Session 混用；重复 offer/answer 返回 `EWRTC_STATE`。

以上协商接口返回 `EWRTC_OK` 只表示输入已复制并入队，异步 SDP 校验或建连失败由 `on_error` 和状态回调报告。answer 的方向、媒体顺序、MID、Payload Type、H.264 profile 和 DTLS setup 必须与 offer 匹配。DTLS 握手完成时验证证书指纹，通过后才创建 SRTP。

收到 answer 之前到达的 candidate 最多暂存 32 条；超限进入 FAILED。SDP 内嵌 IPv4/UDP candidate 和 `end-of-candidates` 也可以使用。SDK 输出本地 SDP 后增量收集 candidate；若对端只接收完整 SDP，业务需要等待收集结束，把候选加入 BUNDLE 对应的媒体段后再发送。candidate 回调可能带 `a=` 前缀，拼接 SDP 时不要重复添加。

## 视频回调与接收策略

```c
void on_video(ewrtc_session *session,
              const uint8_t *annex_b, size_t length,
              uint32_t rtp_timestamp, int keyframe,
              void *user);
```

- 每次回调是一帧完整 Annex-B 访问单元，各 NAL 使用四字节起始码；`keyframe` 表示 AU 含 IDR。
- `rtp_timestamp` 是远端 90 kHz RTP 时间戳，会按 32 位回绕，不是本地微秒时间，也不直接等于音频时间戳。
- 数据只在回调期间有效。业务若要异步解码、写文件或显示，应先复制或交给自己的有界队列；SDK 不提供 H.264 解码器。
- 支持 RFC 6184 packetization-mode=1 的单 NAL、STAP-A、FU-A，以及协商后的 RTX 解封装。
- 接收缓存最多保留 128 个乱序包；缺包等待上限为 150 ms。NACK 最快每 30 ms 一次，PLI 最快每 500 ms 一次，并遵守 SDP 的反馈协商。
- 缺包超时、缺失分片或非法 NAL 会丢弃相应 AU 并请求关键帧。超出窗口的跳跃会丢弃当前 AU，避免把缺少开头的帧当作完整帧交付。
- 单 AU 硬上限为 `EWRTC_MAX_FRAME`（2 MiB），按需分配；额外乱序包缓存约不超过 128 × 2 KiB 加元数据。
- 定期发送视频 RTCP RR/SDES；接收 SR 维护 LSR/DLSR，并统计包损失与到达抖动。音频仍沿用现有逐包回调，不增加音频 jitter buffer 或音视频同步播放器。

新增统计字段：`received_video_packets`、`received_video_frames`、`dropped_video_frames`、`received_rtx_packets`、`sent_nacks`、`sent_plis`。接收包数为去重后接受的原始序列号数量；RTX 另有计数。丢弃帧数是接收端观察到的损坏 AU 数量，无法统计完全未收到任何包的帧。

## 兼容范围和生命周期

- 每个 Session 仍只协商一条 H.264 和一条 Opus 轨道，两个 m-line 都必须保留；不用的方向设置为 inactive。拒绝额外媒体段、移除轨道的 answer 和不匹配的协商结果。
- 仍限 IPv4/UDP、BUNDLE、rtcp-mux、DTLS 1.2、SRTP_AES128_CM_SHA1_80。没有新增 WHIP/WHEP HTTP 信令适配。
- 一次 Session 只支持一次初始协商，不支持重新协商或 ICE restart；重连时销毁并创建新 Session。
- 创建接口已统一要求显式 context，应用须迁移并与静态库一起重新编译；建议使用指定字段初始化 callbacks，配置继续通过 `ewrtc_session_config_init` 初始化。
- 所有业务回调仍在对应 Session 的线程执行；不要在回调内调用 destroy。停止业务投递后，在回调外调用 `ewrtc_session_destroy`；返回后再释放 user 上下文。

## 示例与验证

`examples/demo.c` 增加 `CREATE_OFFER` 和 `ANSWER` 输入记录，`on_local_sdp` 输出对应的 `OFFER`/`ANSWER` 记录。接收视频写入当前目录 `received.h264`。这些文件写入只是验证示例，不建议在产品的会话回调里做阻塞磁盘操作。

从 ewrtc 根目录启动现有信令服务，打开 `/duplex.html?offerer=sdk` 可验证 SDK 主动协商；`offerer=browser` 验证浏览器主动协商。非默认 WebSocket 端口须同时设置 `ws` 查询参数。页面发送 Canvas 视频和 440 Hz 测试音，SDK 发送传入的测试素材。

```bash
cmake --build cmake-build-debug -j8
ctest --test-dir cmake-build-debug --output-on-failure
python3 tests/test_browser_duplex.py \
  --demo cmake-build-debug/ewrtc_demo --video /tmp/input.frames \
  --output /tmp/ewrtc-duplex
```

浏览器测试需要 Chrome、FFmpeg/ffprobe、Python websockets 和 psutil；不会读取用户的摄像头或麦克风，使用独立临时浏览器配置。测试同时验证 Chrome 解码 SDK 视频，以及 SDK 收到的 H.264 文件能被 FFmpeg 解码。

本次本机验证结果见[验证记录](validation-data/receive-offer/validation.md)。

多路线程、队列预算、两阶段关闭和销毁屏障见 [context 接入说明](context.md)。
