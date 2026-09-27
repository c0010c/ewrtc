# 视频接收和主动协商验证（2026-09-25）

- Debug 构建：CTest 9/9 通过（architecture、components、sdp_negotiation、receive、custom_pal、turn、protocol、negotiation、runtime）。
- negotiation 覆盖四种 ICE/DTLS 配置两两互通的 32 组 DTLS 角色组合；另验证单向协商、SDP 内嵌候选、重复协商拒绝、非法 answer 和证书指纹不匹配。
- receive 覆盖单 NAL/STAP-A/FU-A、乱序、RTX、16 位序列号回绕、重复包、NACK 位图、PLI、SR/RR 时间字段、坏包、2 MiB 组帧上限、分配失败和实例隔离。
- Chrome 双向互通 8/8 通过：native/libjuice × OpenSSL/Mbed TLS × SDK/browser offerer。浏览器解码 SDK 视频；SDK 收到的 320×180 H.264 经 ffprobe 统计、FFmpeg 严格解码检查通过，细节见 browser-matrix.json。
- 旧 browser.html + run_matrix.py 的 native-openssl-direct 回归通过（320×180，视频显示及音频内容检查）。
- native/OpenSSL 构建 ASan+UBSan+泄漏检测：CTest 9/9 通过。
- verify_builds.py：四种裁剪后端、独立组件构建、安装后 consumer 链接、聚合库和不包含 Linux PAL 的构建检查通过。

此次新增浏览器互通验证采用本机直连；没有新增公网、长时间持续运行或多路性能结论。已有 TURN 组件测试保留并通过。
