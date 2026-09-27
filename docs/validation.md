# ewrtc 第一阶段 x86_64 验证记录

更新：2026-09-25。范围与接口见[需求文档](requirements.md)及 [SDK 使用说明](../README.md)。本报告中的浏览器、信令服务和测试素材与设备侧 SDK 分开；尚未把本机结果等同于目标嵌入式板卡验收。

## 环境与构建

| 项目 | 实测配置 |
| --- | --- |
| 主机 | Ubuntu 26.04 x86_64；AMD Ryzen 7 7735HS，16 个逻辑 CPU |
| 工具 | GCC 15.2；Chrome 152.0.7977.75；FFmpeg 8.0.1 |
| SDK 依赖 | libSRTP 2.7.0；OpenSSL 3.5.5 或 Mbed TLS 3.6.5；libjuice v1.7.3 提交 `6d0356d092701dcce1f731559d38dc94f52eeb14` |
| 示例独有依赖 | libopus 1.6.1；Python websockets 15.0.1、psutil 7.1.0；本机 coturn |
| 编译 | 四种 ICE × DTLS 组合分别以 Release 构建，未使用的后端不编译；`ctest` 在四种构建均通过。另有 ASan 构建的协议测试与一例 Chrome 互通通过。 |

四种示例执行文件的 `ldd` 均没有 `libstdc++`。SDK 静态库及直接依赖在这台主机上的文件大小如下；这是**文件体积**，没有把共享的 libc、系统加载器等操作系统组件重复相加，也不代表 SDK 独占的运行内存。`libopus` 只由示例链接。

另以 `EWRTC_BUILD_EXAMPLES=OFF`、`EWRTC_BUILD_TESTS=OFF`、仅自有 ICE + OpenSSL 构建产品库，得到 85,414 B 的 `libewrtc.a`；未解析符号中没有 Opus 或 C++ ABI 符号。`libewrtc.a` 的具体字节数会随编译选项和源文件修订变化。

| 裁剪组合 | `libewrtc.a` | 额外静态组件 | 直接动态组件 |
| --- | ---: | ---: | --- |
| 自有 ICE + OpenSSL | 85,414 B | 无 | libSRTP 92,792 B；libssl 1,106,088 B；libcrypto 6,386,528 B |
| 自有 ICE + Mbed TLS | 88,822 B | 无 | libSRTP 92,792 B；Mbed TLS 三个库合计 981,744 B |
| libjuice + OpenSSL | 65,150 B | libjuice 316,528 B | 同 OpenSSL 组合 |
| libjuice + Mbed TLS | 68,710 B | libjuice 316,528 B | 同 Mbed TLS 组合 |

## Chrome 互通矩阵

采用 H.264 Constrained Baseline、无 B 帧、关键帧包含 SPS/PPS；两份 10 秒素材各 300 帧，平均码率分别为 1.001 Mbps 与 1.995 Mbps，按 30 fps 发送。Opus 为 48 kHz、20 ms。矩阵每个场景完成连接、几秒视频解码、双向音频内容检查和释放。直连与强制 TURN/UDP 分别测试。

| ICE | DTLS | 720p30 直连 | 720p30 TURN | 1080p30 直连 | 1080p30 TURN |
| --- | --- | --- | --- | --- | --- |
| 自有 | OpenSSL | 通过 | 通过 | 通过 | 通过 |
| 自有 | Mbed TLS | 通过 | 通过 | 通过 | 通过 |
| libjuice | OpenSSL | 通过 | 通过 | 通过 | 通过 |
| libjuice | Mbed TLS | 通过 | 通过 | 通过 | 通过 |

16/16 场景通过。Chrome 显示的尺寸与素材一致，视频解码帧数增长。浏览器播放设备侧约 600 Hz 测试音；C 端收到并离线解码浏览器约 440 Hz 测试音。强制中继时，自有 ICE 选中本端 relay；libjuice 选中本端 srflx、对端 relay，均包含 TURN 中继路径。本机 coturn 详细日志记录了成功的 `ALLOCATE`、`CREATE_PERMISSION`；自有 ICE 关闭时还记录了 `REFRESH` 释放。逐场景原始数据：[720p](validation-data/matrix-720p.json)、[1080p](validation-data/matrix-1080p.json)、[TURN 服务端摘录](validation-data/turn-server-excerpt.txt)。

测试页面用 `performance.now()` 从点击连接到 `RTCPeerConnection.connectionState=connected` 测连接时间，从点击连接到第一帧 `requestVideoFrameCallback` 测首帧时间。8 个 720p 场景中位数分别为 117.5 ms、148.5 ms；8 个 1080p 场景分别为 116.5 ms、153 ms。这些是同机浏览器及本机 TURN 结果，不能推断公网连接时延。

## 生命周期、弱网与持续传输

- 100 次真实 Chrome 连接、媒体验证、浏览器关闭、C 会话释放与重建连续通过；每次结束后检查示例进程退出。[逐次结果](validation-data/lifecycle-100.json)。另在每种裁剪构建中运行 100 次 API 创建释放循环及 SDP/STUN、非法输入检查。
- 发送队列人为降至 16,384 B 后持续 15 秒，SDK 报告 76 次背压，结束时队列为 0 B，RTX 缓存 239,850 B；示例进程 RSS 在 16.19–16.45 MB 内，连接和双向音频保持可用。关键帧因队列限制被拒绝，浏览器产生 75 次 PLI；该场景专门检验容量控制，不作为视频质量结果。[原始数据](validation-data/backpressure-15s.json)。
- 在 libjuice + OpenSSL 的本机 TURN 会话中分别施加 1% 和 5% 回环丢包各 20 秒：1% 时 NACK/RTX 从 0/0 增至 73/73，5% 时从 73/73 增至 381/418；Chrome 解码帧数在两个区间均持续增加。浏览器主动请求关键帧后，SDK PLI 从 0 增至 1，Chrome 已解码关键帧数从 24 增至 26。脚本在每段测试后移除 `tc` 规则。[原始数据](validation-data/loss-and-pli.json)。
- 在自有 ICE + OpenSSL 直连上施加 100% 回环丢包 35 秒，SDK 报告 `DISCONNECTED`（状态 4）；移除丢包、关闭旧会话并重新发起协商后，Chrome 与 SDK 均恢复到 connected，视频与双向音频检查通过。[原始数据](validation-data/recovery-35s.json)。
- 自有 ICE + OpenSSL 与 libjuice + OpenSSL 的 720p 强制 TURN/UDP 会话各持续 660 秒，均保持连接；Chrome 分别解码 19,900、19,902 帧，已超过 TURN 默认 600 秒分配寿命。原始结果：[自有 ICE](validation-data/turn-soak-native-660s.json)、[libjuice](validation-data/turn-soak-juice-660s.json)。
- 自有 ICE + OpenSSL 的 720p 与 1080p 直连各持续 1800 秒，均保持连接。Chrome 分别解码 54,116、54,115 帧；C 端累计接收约 9.02 万个可解码的 440 Hz Opus 包。[720p 原始结果](validation-data/soak-720p-1800s.json)、[1080p 原始结果](validation-data/soak-1080p-1800s.json)。两档长测在最终 SDP 校验和 ICE 清理小修订前启动，但共用 RTP/RTCP、SRTP 与媒体发送路径未改动；最终修订后的 16 场景短测再次全数通过。

## 资源采样

启动后尚未收到 offer 的示例进程：自有 ICE + OpenSSL RSS 8.17 MB、自有 ICE + Mbed TLS 6.27 MB、libjuice + OpenSSL 8.23 MB、libjuice + Mbed TLS 6.36 MB，均为 3 个线程。libjuice 线程会在建立 ICE 后增加。详见[空闲数据](validation-data/idle-resources.json)。

下表汇总每个组合 5 秒稳定传输采样的**直接与 TURN 两条路径中的较大值**；CPU 栏给出两条路径均值的范围，100% 代表占用一个逻辑核。该短时采样仅用于比较后端，长期基线见持续传输数据。示例进程包含 Opus 编码器与媒体读取，不可把全部 RSS 或 CPU 直接归为 SDK。
测试时多组会话在同一台 16 逻辑核主机上并行运行；最终设备资源上限应以隔离、独占主机或目标板实测制定。

| 组合 | 720p CPU | 720p 峰值 RSS | 1080p CPU | 1080p 峰值 RSS | 最大线程数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 自有 ICE + OpenSSL | 2.0–2.2% | 16.7 MB | 2.2–2.4% | 17.0 MB | 3 |
| 自有 ICE + Mbed TLS | 2.0% | 10.4 MB | 2.2% | 10.7 MB | 3 |
| libjuice + OpenSSL | 2.2–2.6% | 17.0 MB | 2.4–2.8% | 17.3 MB | 4 |
| libjuice + Mbed TLS | 2.0–2.2% | 10.7 MB | 2.4–2.6% | 11.1 MB | 4 |

660 秒 TURN 传输中，自有 ICE + OpenSSL 的示例进程平均 CPU 为 2.26%、峰值 RSS 为 16.64 MB、3 个线程；libjuice + OpenSSL 分别为 2.17%、17.01 MB、4 个线程。短时原始数据：[720p](validation-data/resources-720p-5s.json)、[1080p](validation-data/resources-1080p-5s.json)。

1800 秒直接传输中，720p 示例进程平均 CPU 为 2.03%、峰值 RSS 为 16.63 MB；1080p 分别为 2.17%、16.97 MB，线程峰值均为 3。此处只记录了峰值和平均值，未保留逐秒 RSS 序列，因此不能由这两项单独证明零泄漏；100 次创建释放与 ASan 协议测试提供补充证据。

## 当前限制与下一阶段条件

- 当前直连、TURN 测试在同一台主机和本机 coturn 上完成，尚无跨公网 NAT、真实防火墙及不同网络的路径验证。
- 自有 ICE 当前发布首个可用的非回环 IPv4 host 地址，尚未覆盖多网卡候选优先级、复杂 NAT 拓扑及跨网候选对的互通矩阵。
- 自有 ICE 的角色冲突、持续授权和 TURN 续期已有实现；11 分钟中继传输已覆盖分配寿命边界。尚未建立专门的双 controlling 角色冲突互通用例。
- 资源统计基于 `ewrtc_demo` **整个进程**，包含示例 Opus 编码、素材读取和 SDK；Chrome、信令 Python 进程、coturn 单独运行。CPU 百分比按 psutil 的单逻辑核 100% 计，1 秒采样；需要在目标板上重新测试并制定硬性上限。
- ARM 工具链、设备编码器输入、录放音及声学回声控制、公网部署、其他浏览器与原生 App 尚未验证。首轮范围仅为 IPv4/UDP，未实现 TURN/TCP、TURN/TLS、动态码率或自动 ICE restart。
- 尚未单独施加受控延迟、抖动和带宽限制；这些网络条件需在目标部署环境补测。
