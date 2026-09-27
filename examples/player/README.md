# Chrome 直接播放示例

从仓库根目录运行：

先安装 [构建依赖](../../README.md#快速开始) 及 FFmpeg（含 libx264），然后：

```bash
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r examples/requirements.txt
python3 examples/run_player.py
```

打开 <http://127.0.0.1:8080/player/?ws=8765>，页面自动建立 WebRTC 连接并静音播放，无需摄像头、麦克风或浏览器插件。点击“开启声音”可听到 600 Hz Opus 测试音。刷新或“重新连接”会新建 SDK 会话；关闭页面会释放会话。

脚本自动构建 native ICE + OpenSSL 的 `ewrtc_demo`，通过 FFmpeg 自动生成 Baseline H.264 720p30 测试画面，随后循环推流。需要项目构建依赖、FFmpeg（libx264）、Python 3 和 `websockets`。使用系统依赖版本；不需要下载第三方媒体。

```bash
# 更换视频或端口（视频统一转换为 720p30，最多取前 10 秒）
python3 examples/run_player.py --source /path/to/video.mp4 --http-port 8090 --ws-port 8767
```

脚本默认前台运行，Ctrl+C 停止。构建、媒体和运行输出位于 `build-player/`。地址仅供运行服务的本机访问。

页面的“连接阶段耗时”显示 WebSocket、Offer、本地 SDP、等待 Answer、远端 SDP、ICE 收集/检查、DTLS、建连总计和首帧耗时；每次重连清零。所有测量使用浏览器单调时钟，累计值从开始连接算起。并行阶段不可相加；等待 Answer 包含服务端处理与信令传输，DTLS 取浏览器状态事件，首帧取 `requestVideoFrameCallback`，不代表单独的服务端或解码器耗时。计时记录与表格展示独立在 `timings.js` 中。

模块分工：`run_player.py` 负责构建与素材准备；复用 `signaling.py` 处理 HTTP/WebSocket 和 `demo.c` 管道；`client.js` 管理 WebRTC 会话；`app.js` 管理播放器界面、状态和统计。视频走 SDK 的 SRTP/UDP 链路，HTTP 仅提供页面。
