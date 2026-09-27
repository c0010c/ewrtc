# ewrtc：嵌入式纯 C WebRTC SDK（共享 context 与事件驱动）

所有命令均从仓库根目录执行。本文说明设备侧 C SDK。支持设备或 Chrome 发起 offer，协商一条 H.264 和一条 Opus 轨道，音视频均可配置发送、接收或双向；首轮只支持 IPv4/UDP、BUNDLE、RTCP mux、trickle ICE、DTLS 1.2 与 `SRTP_AES128_CM_SHA1_80`。默认后端为自有 ICE + OpenSSL。设备侧不链接 Google WebRTC 的 C++ 代码。浏览器页面、信令服务及素材转换脚本只用于验证，不属于设备运行依赖。

## 模块与 PAL

SDK 内部拆成 Common、PAL、Linux PAL、Crypto、SDP、STUN、TURN、ICE、DTLS、SRTP、RTP/RTCP、Media 和 Session，各模块保留独立构建与测试。对外只提供业务接口 `include/ewrtc.h`、必要的公共类型/后端选择，以及 `include/ewrtc/pal*.h`、`include/ewrtc/pal/` 和平台提供者组成的平台适配接口。协议接口位于 `src/<模块>/<模块>.h`，不安装、不作为外部组件导出。模块依赖、PAL 服务契约、驱动和第三方限制见[架构说明](modular-architecture.md)。多路设计与新接口见[context 接入说明](context.md)，context 阶段测试见[context 验证记录](context-validation.md)。历史重构结果见[重构验证记录](modular-validation.md)。

应用创建显式 context 并注入 PAL，再用 `ewrtc_session_config_init` 初始化每路配置。`crypto_backend` 与 `dtls_backend` 分开配置。协议组件通过 PAL 使用平台能力，协议状态与超时策略留在组件内部。内部组件由 Session 串行驱动，完整 SDK 使用 context 共享 I/O worker 和准备线程。

```c
#include <ewrtc.h>
#include <ewrtc/platform/linux.h>

ewrtc_context_config runtime;
ewrtc_context_config_init(&runtime);
runtime.pal = *ewrtc_pal_linux();
ewrtc_context *context = NULL;
/* 检查每次返回值；此处省略业务回调和协商。 */
ewrtc_result result = ewrtc_context_create(&runtime, &context);
ewrtc_session_config config;
ewrtc_session_config_init(&config);
/* ewrtc_session_create(context, &config, &callbacks, user, &session); */
/* 所有 session_destroy 完成后，才调用 ewrtc_context_destroy(context)。 */
```

开发 SDK 时，可以只构建和测试内部 SDP、RTP/RTCP 模块（不生成对外安装包）：

```bash
cmake -S . -B build-protocols -DEWRTC_COMPONENTS='sdp;rtp'
cmake --build build-protocols -j
ctest --test-dir build-protocols --output-on-failure
```

## 构建

历史验证环境（降级前）：Ubuntu 26.04 x86_64、GCC 15.2、CMake、pkg-config、OpenSSL 3.5.5、Mbed TLS 3.6.5、libSRTP 2.7.0、libjuice v1.7.3 的提交 `6d0356d092701dcce1f731559d38dc94f52eeb14`。示例另需 Opus 1.6.1；浏览器验证另需 Chrome 152、FFmpeg 8、coturn、Python `websockets` 15.0.1 和 `psutil` 7.1.0。当前 OpenSSL 已固定为 1.1.1w，准备步骤见 [构建说明](building.md)，降级验证见 [验证记录](openssl-1.1.1w-validation.md)。设置 `EWRTC_ENFORCE_DEPENDENCY_LOCK=ON` 可额外锁定 Mbed TLS / libSRTP 版本并校验 libjuice 提交。上述浏览器记录仍属于降级前环境。

```bash
cmake -S . -B build-native-openssl -DCMAKE_BUILD_TYPE=Release \
  -DEWRTC_WITH_NATIVE_ICE=ON -DEWRTC_WITH_LIBJUICE=OFF \
  -DEWRTC_WITH_OPENSSL=ON -DEWRTC_WITH_MBEDTLS=OFF -DEWRTC_BUILD_EXAMPLES=ON
cmake --build build-native-openssl -j
ctest --test-dir build-native-openssl --output-on-failure
```

将 `NATIVE_ICE` 与 `LIBJUICE`、`OPENSSL` 与 `MBEDTLS` 两组选项互换即可构建另外三种裁剪组合。示例默认关闭；`EWRTC_BUILD_EXAMPLES=ON` 启用依赖 Opus 的测试程序。公开头文件为 [include/ewrtc.h](../include/ewrtc.h)。Linux 应用链接 `ewrtc` 和 `ewrtc_pal_linux`，CMake 传递各自依赖。Session 和聚合库本身均不包含 Linux 提供者。产物是静态 `libewrtc.a`；系统加密库及 libSRTP 当前采用动态链接。libjuice 后端额外静态链接其 C 库。库本身不使用 libopus，业务负责 Opus 编解码。

`cmake --install build-native-openssl --prefix <安装目录>` 只安装公开头文件、聚合 `lib/libewrtc.a`、启用时的 `lib/libewrtc_pal_linux.a` 和 CMake package；不安装内部协议头文件和组件库；脱离 CMake 目标直接使用静态库时，业务构建还需显式链接该组合所需的 libSRTP、加密库和 pthread，libjuice 组合另需链接对应静态库。Linux 应用使用：

```cmake
find_package(ewrtc CONFIG REQUIRED COMPONENTS session pal_linux)
target_link_libraries(my_app PRIVATE ewrtc::session ewrtc::pal_linux)
```

也可以将 `ewrtc::session` 换成聚合 target `ewrtc::ewrtc`。自定义平台用 `EWRTC_WITH_LINUX_PAL=OFF` 构建，并注入自有 PAL；native 后端下无需链接 pthread。第三方后端内部平台依赖不由 PAL 接管。

已安装包的公开 CMake targets 只有 `ewrtc::ewrtc`、`ewrtc::session`（指向完整 SDK）和启用时的 `ewrtc::pal_linux`。STUN/TURN 服务器配置、SDP 和 candidate 交换仍通过 Session 完成。`backends.h` 只声明配置/统计使用的枚举，不公开协议操作。

RTP、TURN 示例保留为 SDK 内部开发示例，使用源码根目录构建：

```bash
cmake -S . -B build-internal-examples -DEWRTC_BUILD_INTERNAL_EXAMPLES=ON
cmake --build build-internal-examples -j
```

这些示例使用内部头文件，不支持从已安装 SDK 独立构建。公开边界和变更后的验证说明见 [API 边界说明](public-api.md)。

## 接口约定

- `ewrtc_session_create(context, …)` 复制配置并固定绑定 context 的 worker，不创建单路线程；PoC 允许通过配置选择 ICE、DTLS 后端。每个会话支持一次初始协商，可接收远端 offer，或通过 `ewrtc_session_create_offer` 主动发起并通过 `ewrtc_session_set_remote_answer` 接收 answer；重连需关闭、释放并创建新会话。
- `ewrtc_session_set_remote_offer`、`ewrtc_session_add_remote_candidate` 和 `ewrtc_session_end_remote_candidates` 只将信令复制到队列。answer、增量本地 candidate 和收集结束由回调送给业务层；业务层负责实际信令运输。
- `ewrtc_session_send_video` 接收一个完整 Annex-B 访问单元，`pts_us` 为微秒，关键帧标记必须与 IDR 一致，IDR 访问单元必须携带 SPS/PPS。首轮要求 H.264 Constrained Baseline、无 B 帧。`ewrtc_session_send_audio` 接收一个完整 Opus 包和微秒时间戳，单包上限由 `EWRTC_MAX_OPUS_PACKET_BYTES` 给出；首轮使用 48 kHz、20 ms 包。
- 发送函数同步复制整个输入；默认单路普通队列上限 1 MiB，包含节点和正在处理的任务；context 总预算还包含每路控制预留。整帧不能入队时返回 `EWRTC_BACKPRESSURE`，调用方保留原始输入所有权。视频 RTX 缓存默认最多 2 MiB、最长 2000 ms；两项均可配置。统计通过 `ewrtc_session_get_stats` 获取快照。
- 内部关键事件入队失败会报告 NOMEM/BACKPRESSURE，进入 FAILED 并释放组件后进入 CLOSED。close 不分配队列内存；发送暂不可用或背压不自动重放已部分发送的帧。
- 同一路所有业务回调在所属 context worker 串行执行，不允许长时间阻塞。回调传入的 SDP、candidate、H.264/Opus 数据和错误文本仅在调用期间有效。回调中可以提交输入或读统计；任何 session/context 的同步 destroy 都必须在所有 SDK 业务回调之外调用。session destroy 等待两阶段清理、后端生产者退出和最终 CLOSED 回调返回；同步 DNS 可延长等待，其他流的 I/O 不受此等待阻塞。
- `relay_only=1` 需要 TURN 地址和凭据。自有 ICE 仅公布和选用 relay 候选；libjuice 在本地 TURN 配置下可能选择本端 srflx 到对端 relay 的候选对，SDK 会检查选中路径包含 relay。候选对见统计中的 `local_candidate` 与 `remote_candidate`。

## 视频接收与主动 offer

新增 `on_video` 回调交付完整 Annex-B 视频 AU；支持 H.264 单 NAL/STAP-A/FU-A、乱序整理、RTX、NACK/PLI 和视频接收统计。主动建连使用 `ewrtc_session_create_offer` / `ewrtc_session_set_remote_answer`；原有远端 offer 路径保留。`video_direction`、`audio_direction` 配置每条轨道的本端方向。

接口时序、缓存上限、回调所有权、兼容范围和双向 Chrome 示例见[接入说明](receiving-and-offers.md)。

## 本地 Chrome 验证

### 一键打开播放示例

在本仓库根目录执行 `python3 examples/run_player.py`，然后在 Chrome 打开
[http://127.0.0.1:8080/player/?ws=8765](http://127.0.0.1:8080/player/?ws=8765)。
脚本自动构建 SDK、准备 720p30 视频并启动服务，页面自动静音播放。
模块说明、依赖和参数见 [播放器示例](../examples/player/README.md)。

先生成可循环读取的视频访问单元。以下两档分别为 720p30/1 Mbps 和 1080p30/2 Mbps；关键帧每 60 帧重复 SPS/PPS。示例读取 `.frames` 文件并发送 600 Hz Opus 测试音，浏览器发送 440 Hz 音频，C 示例将收到的 Opus 包写入 `received.opuspkts`。

```bash
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 10 -an \
  -c:v libx264 -preset ultrafast -b:v 1M -maxrate 1M -bufsize 2M \
  -pix_fmt yuv420p -profile:v baseline -level:v 3.1 \
  -x264-params keyint=60:min-keyint=60:scenecut=0:bframes=0:repeat-headers=1:aud=1 \
  -f h264 /tmp/ewrtc-720p.h264
python3 examples/prepare_media.py /tmp/ewrtc-720p.h264 /tmp/ewrtc-720p.frames
python3 examples/signaling.py --ice native --dtls openssl \
  --demo build-native-openssl/ewrtc_demo --video /tmp/ewrtc-720p.frames
```

打开终端输出的浏览器地址并点击 Start。强制 TURN/UDP 的本机测试服务及矩阵脚本：

```bash
turnserver -n --no-cli --no-tcp --no-tls --no-dtls --no-tcp-relay \
  --listening-ip=127.0.0.1 --relay-ip=127.0.0.1 --realm=ewrtc.local \
  --lt-cred-mech --user=test:testpass --allow-loopback-peers \
  --listening-port=3479 --min-port=49160 --max-port=49260
python3 examples/run_matrix.py --video /tmp/ewrtc-720p.frames \
  --width 1280 --height 720 --output-dir /tmp/ewrtc-matrix-720p
```

本机 TURN 的 loopback 配置仅供测试。矩阵脚本以独立 Chrome/CDP 进程检查视频尺寸、逐帧解码、双向音频内容和实际候选路径。`--soak-seconds 1800 --only native-openssl-direct` 可对一档执行 30 分钟持续传输与示例进程资源采样；`repeat_connections.py --video /tmp/ewrtc-720p.frames --cycles 100` 检查完整连接、关闭和重建循环。结果和当前限制见 [验证记录](validation.md)。

`test_backpressure.py --video /tmp/ewrtc-720p.frames` 用 16 KiB 队列验证背压。丢包、关键帧请求及断网重建通过隔离脚本执行：

```bash
tests/run_isolated_network.sh /tmp/ewrtc-720p.frames /tmp/ewrtc-network-validation
```

该脚本创建临时 user/network namespace 和虚拟接口，只修改该命名空间中的 `lo` 排队规则；测试程序拒绝直接修改宿主机回环网络。环境需要允许 user namespace，并提供 `ip`、`tc`、coturn 和 Chrome。

完整构建与组件集成验证：

```bash
python3 tests/verify_builds.py
python3 tests/check_architecture.py
```

已有 libjuice checkout 时，可给构建验证脚本传 `--juice-source <路径>`，避免重复下载。依赖提交仍会按锁定版本校验。
