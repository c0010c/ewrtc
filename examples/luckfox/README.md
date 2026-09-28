# Luckfox Pico Plus / RV1103 板载摄像头

Chrome 打开 <http://127.0.0.1:8081/player/?ws=8766&source=camera>。
这是 SC3336 摄像头的实时画面，1280×720、25 fps；当前只有视频。

## 数据路径与模块

摄像头 → 板上 ISP / 硬件 H.264 编码 → 板内 RTSP → `ewrtc_camera` →
SRTP/UDP → Chrome。媒体不经过电脑转码；电脑只提供页面和 SSH 信令转发。

- `configure_camera.py`：从原厂配置派生独立的 720p H.264 Baseline 配置，
  关闭多余的子码流、NPU、IVS、JPEG、音频和 OSD，节省板端内存。
  默认周期 GOP 为 25 帧（1 秒），配合按需硬件 IDR；未启用控制模块时可用
  `--gop 5` 将周期缩至 200 ms，但关键帧比例和实际带宽开销会增加。
- `camera_source.c`：只连接板内 `rtsp://127.0.0.1/live/0`，完成 RTSP 协商和
  TCP interleaved 接收；此示例不支持外部 RTSP、鉴权和其他编码。
- `camera_h264.c`：还原单 NAL、STAP-A、FU-A，缓存 SPS/PPS 并随 IDR 交付，
  检查序号、包长和帧容量；将 90 kHz 时间戳转为 SDK 的微秒时间戳。
- `camera_app.c`：常驻进程入口，只启动一个摄像头读取线程；通过文件锁防止重复启动。
- `session_hub.c`：一个 SDK context 管理多个 session，共享一个 worker 和一个准备线程。
  单份摄像头帧分发到已连接的会话；连接完成、PLI 和发送背压后请求硬件 IDR，
  各会话等待收到可解码的 IDR 后继续发送。每份 IDR 都带 SPS/PPS。
  默认允许 4 个会话，最多配置 8 个；每个会话发送队列和重传缓存各限 128 KiB。
- `idr_control.c` / `idr_protocol.h`：非阻塞本地数据报控制；SDK 回调不等待硬件操作。
- `rkipc_idr_bridge.c`：在原厂 `rkipc` 内加载的小型模块，动态解析并调用
  `RK_MPI_VENC_RequestIDR(0, RK_FALSE)`，由拥有编码通道的进程请求下一帧 IDR。
  固定控制通道 0，socket 权限 0600，多会话请求合并，硬件调用间隔至少 100 ms。
  不创建、重置或销毁编码器；原厂可执行文件不作修改。模块增加一个控制线程，
  WebRTC 进程仍固定 5 个线程。控制模块不可用时会话仍等待正常周期 IDR。
- `mux_io.c`：带会话 ID 的管道协议，格式为 `id TYPE length\n` 后接 payload。
  ID 0 用于进程消息；SDK 回调写入有界队列，由专用线程输出，避免阻塞共享 worker。
- `examples/multiplex_signaling.py`：所有 WebSocket 复用一个 SSH 子进程，按 ID 路由。
  每个浏览器有独立的有界输出队列，慢客户端只断开自身；后端断线则关闭所有会话，
  由 systemd 重启整个信令服务。原 `signaling.py` 仍用于单会话文件播放示例。
- `run_demo.py`：SSH 启动板端程序，`EWRTC_DEVICE_PROGRAM=ewrtc_camera` 选择摄像头应用。
- `cmake/luckfox-rv1103.cmake` / `build.sh`：ARM uClibc 工具链和静态依赖构建。

## 部署配置

本示例适配 Buildroot / Linux ARMv7 固件。先在自己的板卡上确认摄像头、
RTSP 地址、固件 ABI 和 SSH 访问。将构建产物部署到 `/userdata/ewrtc/`，
使用 `configure_camera.py` 从自己的原厂配置生成 `camera.ini`，保留原始配置备份。

从仓库根目录复制并填写设备配置（该文件被 Git 忽略）：

```bash
cp examples/luckfox/device.env.example .env.luckfox
# 编辑 .env.luckfox，填写实际主机与密钥
. ./.env.luckfox
python3 -u examples/multiplex_signaling.py \
  --demo "$PWD/examples/luckfox/run_demo.py" \
  --max-sessions 4 --http-port 8081 --ws-port 8766
```

`EWRTC_DEVICE_HOST` 必填，可使用 SSH config 中的主机别名。
`EWRTC_SSH_KEY` 可选，未设置时沿用 SSH 默认身份。
`EWRTC_DEVICE_DIR` 默认 `/userdata/ewrtc`；`EWRTC_DEVICE_PROGRAM=ewrtc_camera`
选择摄像头程序。板卡没有持久 RTC 时，摄像头启动脚本会通过 SSH 同步主机 UTC 时间。

板端启动摄像头前先停止旧 `rkipc`，等待它退出后再替换配置，避免退出时写回旧配置。
然后按当前固件提供的 IQ 文件位置执行：

```bash
LD_LIBRARY_PATH=/oem/usr/lib LD_PRELOAD=/userdata/ewrtc/libewrtc_idr.so \
  nohup /oem/usr/bin/rkipc -c /userdata/ewrtc/camera.ini \
  -a /oem/usr/share/iqfiles >/tmp/ewrtc-camera.log 2>&1 </dev/null &
```

可选的持久化配置：

- `99-ewrtc-luckfox.rules` 是模板，安装前用自己的 USB 序列号替换 `YOUR_DEVICE_SERIAL`。
- `S99ewrtc-network` 从板端 `/etc/default/ewrtc-network` 读取 `EWRTC_USB_ADDRESS`，
  例如 `192.168.50.2/24`；主机应配置同一子网且不同的地址。配置后再接入板端启动流程。
- `start_camera.sh` 可接入固件摄像头启动分支。先备份原厂启动脚本；不同固件需自行核对。

关闭页面只销毁该页 session，其他页面和共享采集进程继续运行。
会话上限不是稳定播放路数保证，应按设备 CPU、内存和网络实测设置。
`idr_requests` / `idr_send_errors` 是请求统计，不等同于硬件确认；
首帧延迟应以浏览器计时为准。示例不会自动安装系统服务或修改网络。

## 构建和验证

将 [Rockchip GCC 8.3 uClibc 工具链](https://files.luckfox.com/wiki/Luckfox-Pico/Software/arm-rockchip830-linux-uclibcgnueabihf.tar.gz) 解压到 `build-luckfox/`，或通过 `LUCKFOX_TOOLCHAIN` 指定其目录。依赖源码从主仓库锁定的 submodule 读取，不再手工下载压缩包。需要 CMake 3.21+、Git、Python 3、tar 和 make。

```bash
git submodule update --init third_party/libsrtp third_party/opus
git submodule update --init --recursive third_party/mbedtls
bash examples/luckfox/build.sh
python3 examples/luckfox/configure_camera.py original.ini camera.ini
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  examples/luckfox/camera_h264.c examples/luckfox/test_camera_h264.c \
  -o build-luckfox/test_camera_h264
build-luckfox/test_camera_h264
```

产物为 `build-luckfox/sdk/ewrtc_camera`、`libewrtc_idr.so` 和原素材示例 `ewrtc_demo`。
摄像头应用不链接 Opus，不做软件视频编码。Mbed TLS 开启 DTLS-SRTP，版本锁定仍启用。
摄像头采集复用当前固件的 `/oem/usr/bin/rkipc` 和 `/oem/usr/lib`。

以下为 2026-09-26 的历史板卡验证摘要，未关联 Git 提交。Chrome 持续播放超过一分钟及重连已验证，720p25；首帧优化实测见下文。
原始输出仅保存在验证者本地，不随源码发布：
`build-luckfox/camera-validation.json` 保存浏览器验证结果；
`build-luckfox/camera-sdk/camera-probe.json` 保存摄像头编码信息。
H.264 测试覆盖参数集、分片 IDR、序号丢失、时间戳回绕和畸形输入，ASan/UBSan 通过。

多会话回归：`test_mux_protocol.py <本机编译的 camera_app>` 检查容量、重复 ID、
100 次会话创建/销毁、畸形记录及输出管道阻塞时退出。
`test_camera_mux.py` 使用专用调试 Chrome（9239 端口）和当前板端服务，
检查双页面解码、4 会话上限、20 次连接切换以及关闭/重连隔离；运行前关闭其他观看页面。
验证结果写入 `build-luckfox/camera-mux-validation.json`。
双页面实测均为 1280×720、25 fps，始终复用同一个 PID，固定 5 个进程线程
（其中 SDK 2 个）。关闭一个页面、重新加入及关闭所有页面后
再次连接均未重启进程，另一页面持续解码。4 会话容量检查、20 次浏览器信令连接切换、
100 次本机会话生命周期和 ASan/UBSan 应用层异常输入测试已通过。

首帧优化：专用 Chrome 同环境各测 12 次重新连接，分辨率、帧率和码率配置保持不变，
未引入历史帧回放缓存。当前使用第三种策略：

| 策略 | 首帧中位数 | 样本最慢 | 建连后呈现等待中位数 |
| --- | ---: | ---: | ---: |
| 周期 GOP 25，无按需请求 | 625.4 ms | 1134.2 ms | 499.1 ms |
| 周期 GOP 5，无按需请求 | 244.0 ms | 322.9 ms | 125.6 ms |
| 周期 GOP 25，连接完成立即请求硬件 IDR | 171.1 ms | 201.6 ms | 44.4 ms |

实际 RTSP 码流在请求期间出现了 3、10、12 等小于周期 25 的关键帧间隔，
结合硬件接口 `result=0` 和浏览器计时确认请求生效。短 GOP 的测试画面带宽约
0.65 Mbps，恢复 GOP 25 后可避免持续以 5 Hz 插入关键帧；实际码率随画面及请求频率变化。
测量脚本为 `benchmark_first_frame.py`，使用专用 Chrome（9239 端口）：

```bash
python3 examples/luckfox/benchmark_first_frame.py --samples 12 \
  --output build-luckfox/first-frame-hardware-idr.json
```

原始结果为 `build-luckfox/first-frame-before.json`、`first-frame-after.json` 和
`first-frame-hardware-idr.json`。
这些是当前局域网与画面下的实测值，不是所有网络环境的延迟上限。

控制模块本机测试：

```bash
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
  examples/luckfox/rkipc_idr_bridge.c -ldl -lpthread -o build-luckfox/libewrtc_idr_native.so
cc -std=c11 -Wall -Wextra -Werror -rdynamic \
  examples/luckfox/test_rkipc_idr_mock.c -o build-luckfox/test_rkipc_idr_mock
python3 examples/luckfox/test_idr_bridge.py \
  build-luckfox/libewrtc_idr_native.so build-luckfox/test_rkipc_idr_mock
```

测试覆盖畸形数据报、请求突发合并、100 ms 限频、重复实例不抢占 socket 及退出清理。
板端还通过了控制 socket 临时不可用的回退测试：新会话等待周期 IDR 后正常播放，
已有会话继续解码，恢复控制通路后再次连通的会话 13 ms 内获得首个 IDR 入队。
该测试结果在 `build-luckfox/hardware-idr-fallback.json`，其中一次 `idr_send_errors`
增长来自主动故障注入。双页面及 20 次连接切换回归也已在硬件 IDR 版本重新通过。
硬件接口依据 [Luckfox 官方 MPI 头文件](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/media/rockit/rockit/mpi/sdk/include/rk_mpi_venc.h)
及官方 `sample_venc_stresstest.c` 的调用方式。此桥接模块适配当前 ARM32 固件的 C ABI；
升级固件后应重新核对接口并验证硬件返回值与实际码流。
