# ewrtc

[English](README.md) | **简体中文**

面向嵌入式设备的模块化 C11 WebRTC SDK。将设备已经编码的 H.264 / Opus 音视频通过 WebRTC 发送到浏览器，也支持接收媒体和设备主动发起连接。

设备侧不依赖 Google WebRTC 的 C++ 库。平台能力通过 PAL 注入，信令传输由应用实现。默认使用 native ICE + OpenSSL；可选 libjuice 和 Mbed TLS 后端。

**当前版本：0.2.0，实验阶段。** API 仍可能在次版本中变化。已记录 Linux x86_64 / Chrome 本机互通与 Luckfox RV1103 摄像头示例；公网 NAT、多浏览器及其他平台的覆盖仍需完善。详见 [支持范围](docs/support.md)。

## 能力

- H.264 / Opus 各一条轨道，发送、接收或双向；支持本端或远端 offer。
- IPv4 / UDP、BUNDLE、RTCP mux、trickle ICE、TURN/UDP。
- DTLS 1.2、SRTP_AES128_CM_SHA1_80；视频 NACK / RTX / PLI。
- 显式 context 共享 worker，有界队列、背压和会话统计。
- 独立协议模块、平台适配层和可裁剪后端。

目前不支持 IPv6、DataChannel、TURN/TCP、TURN/TLS、自动 ICE restart 或动态码率控制。H.264 使用 Constrained Baseline、无 B 帧，IDR 必须携带 SPS/PPS；应用负责音视频编解码。

## 快速开始

以下命令均在克隆后的仓库根目录执行。需要 Linux、C11 编译器、CMake 3.20+、Make、pkg-config、OpenSSL 3.x 和 libSRTP 2.x（构建下限 2.5）。Python 3.10+ 用于架构及开发检查。构建下限不代表其后的每个版本都已验证，版本记录见 [构建说明](docs/building.md)。

Ubuntu / Debian 上安装默认构建依赖：

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config libssl-dev libsrtp2-dev python3
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default
```

默认生成 `build-default/libewrtc.a`、Linux PAL 静态库和测试，不需要 Opus、Mbed TLS、Chrome 或下载 libjuice。仅构建库可添加 `-DEWRTC_BUILD_TESTS=OFF`。

安装并运行最小接入示例：

```bash
cmake --install build-default --prefix "$PWD/install"
cmake -S examples/minimal -B build-minimal -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build-minimal --parallel
./build-minimal/ewrtc_minimal
```

该示例演示 context / session 的创建与释放；实际媒体传输见播放示例。应用通过 CMake 接入：

```cmake
find_package(ewrtc CONFIG REQUIRED COMPONENTS session pal_linux)
target_link_libraries(my_app PRIVATE ewrtc::session ewrtc::pal_linux)
```

## 浏览器播放示例

另外安装 `libopus-dev`、FFmpeg（含 libx264）、Python venv 和 Chrome：

```bash
sudo apt-get install libopus-dev ffmpeg python3-venv
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r examples/requirements.txt
python3 examples/run_player.py
```

在 Chrome 打开 [本机播放器](http://127.0.0.1:8080/player/?ws=8765)。脚本自动构建示例并生成测试画面，无需摄像头或外部媒体；Ctrl+C 停止。参数和模块说明见 [播放器文档](examples/player/README.md)。板载摄像头接入见 [Luckfox 示例](examples/luckfox/README.md)。

## 架构

```text
应用：信令 / 编解码 / 设备采集
                 │ 公共 API
              Session ─── Context / 有界任务队列
                 │
       SDP / ICE / DTLS / SRTP / Media
              │                  │
          STUN / TURN         RTP / RTCP
                 │
           PAL 服务契约
                 │
       Linux 提供者 / 自定义平台
```

示意图展示分层；精确依赖见 [模块架构](docs/modular-architecture.md)。协议头文件留在 `src/`，外部应用只使用 `include/` 下的公共接口。Linux PAL 与 SDK 分开链接，板卡采集和浏览器信令位于 `examples/`。

## 文档

| 需求 | 入口 |
| --- | --- |
| 构建选项、后端、交叉编译 | [构建说明](docs/building.md) |
| API、线程、内存所有权与信令约定 | [接入指南](docs/usage.md)、[context](docs/context.md) |
| 视频接收、双向媒体和主动 offer | [接收与协商](docs/receiving-and-offers.md) |
| 模块边界与平台适配 | [架构](docs/modular-architecture.md)、[公开 API](docs/public-api.md) |
| 支持状态与历史测试证据 | [支持范围](docs/support.md) |
| 开发、测试与提交修改 | [贡献指南](CONTRIBUTING.md) |
| 发布变化、安全问题 | [变更记录](CHANGELOG.md)、[安全政策](SECURITY.md) |

## 许可证

本项目自有代码采用 [MIT License](LICENSE)。第三方依赖、可选媒体和厂商软件分别遵循原许可证，见 [第三方说明](THIRD_PARTY_NOTICES.md)。
