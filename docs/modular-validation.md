# 协议组件与平台分层重构验证

> 这是对应重构阶段的历史验证记录。此后的公开接口已收窄，独立协议组件不再安装或对外导出；当前规则见 [API 边界说明](public-api.md)。

此文记录 context 改造之前的历史结果；当前线程与队列行为见 [context 接入说明](context.md)，新验证见 [context 验证记录](context-validation.md)。

日期：2026-09-25。范围为当前 `sdk/` 的模块重划、显式 PAL 注入、独立 TURN、Media/Session 拆分及错误处理。设备运行依赖仍为纯 C；未链接参考 Google WebRTC。

## 实现结果

- 平台服务按组声明；平台提供者使用专用头，Linux 实现移至平台目录。Session 和聚合库不再引用 Linux 提供者；应用显式注入并链接平台。
- PAL 按实际能力校验，包括 allocator/resize、单调时钟/UTC、mutex/thread/condition、网络各项。libjuice 的适配队列只要求互斥量；TURN 不要求完整网络表。
- `ewrtc::turn` 可独立构建、安装、消费，管理认证、事务、权限、续期和中继报文；native ICE 借用同一 socket 接入，libjuice 不依赖此组件。
- Crypto 后端实现分文件；Media 按打包、反馈、缓存拆分；Session 按公共 API、运行时、连接、统计拆分。
- 内部事件投递失败保持原始错误并终止会话；close 不再依赖内存分配。第三方状态码在适配边界转换。媒体与 DTLS 保留暂不可发送的语义。

## 已通过的检查

| 检查 | 结果与证据 |
|---|---|
| 四后端构建与 CTest | native/libjuice × OpenSSL/Mbed TLS 全部通过 |
| 独立组件 | 13 个组件独立构建、安装和外部消费全部通过；新增 TURN |
| 聚合库消费 | 外部程序链接 `ewrtc::ewrtc` 通过 |
| 自定义 PAL | 关闭 Linux PAL，使用测试调度器运行完整 Session；聚合库未引用 Linux PAL 或 pthread 符号 |
| 架构边界 | 平台调用、组件依赖、第三方适配位置、私有头与 Session 反向依赖检查通过 |
| 模拟 TURN | 认证、签名/未签名 438、乱序与重复响应、权限按 IP 隔离、重试、超时、续期、内存/DNS/随机/发送失败、借用 socket 生命周期通过 |
| 内部事件失败 | 分配失败与 4096 项队列耗尽均报告具体错误；业务回调处于会话线程，之后完成关闭 |
| Media 错误传播 | RTP、RTCP SR 与 RTX 的 AGAIN/BACKPRESSURE/IO 路径测试通过 |
| ASan/UBSan | native + OpenSSL 的全部 6 项 CTest 通过，开启泄漏检测；无 sanitizer 报错 |
| Chrome 720p30 | 4 后端 × 直连/中继，8/8 通过；验证解码与双向音频内容 |
| Chrome 1080p30 | 同上，8/8 通过 |
| 丢包和关键帧 | 隔离网络命名空间内 1%、5% 丢包及 PLI 通过；NACK/RTX 增长，恢复解码 |
| 断网恢复 | 隔离网络命名空间内断网 35 秒、报告断开、重新创建会话后恢复通过 |
| 生命周期 | Chrome 连续创建/关闭 20 次，20/20 通过 |
| 背压 | 16 KiB 业务发送队列、15 秒压力测试通过 |
| TURN 续期传输 | native + OpenSSL 的 Chrome 强制中继额外运行 65 秒，跨过分配与权限续期时点，仍保持媒体传输 |
| 独立 TURN 示例 | 安装后构建，向本地 coturn 申请 relay 并通过独立 UDP echo 对端收回 16 字节数据 |

原始数据位于 [platform-refactor](validation-data/platform-refactor/)，包括两档互通矩阵、弱网、重建、生命周期、背压 JSON 与构建和 sanitizer 日志。早期验证数据保留在其他目录，不计入本次结果。

## 复现入口

```bash
python3 tests/check_architecture.py
python3 tests/verify_builds.py --output build-refactor-matrix \
  --juice-source cmake-build-debug/_deps/libjuice-src

cmake -S . -B build-refactor-asan \
  -DEWRTC_WITH_LIBJUICE=OFF -DEWRTC_WITH_MBEDTLS=OFF \
  -DEWRTC_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-refactor-asan -j8
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-refactor-asan --output-on-failure
```

互通使用 `/tmp/ewrtc-720p.frames`、`/tmp/ewrtc-1080p.frames`。素材生成、TURN 示例和 Chrome 脚本用法见 [SDK README](../README.md)。本轮主机上的临时 coturn 使用 UDP 3489；测试脚本通过 `--turn-port 3489` 和独立端口偏移运行。弱网脚本使用单独 user/network namespace，不修改宿主机网络。

## 验证边界

上述结果来自当前 x86_64 Linux 和本地 Chrome/coturn，不代表公网跨 NAT、目标嵌入式硬件或其他 OS/RTOS 已验收。第三方内存、随机源、线程与网络仍由各库自管。本轮没有重做早期的 30 分钟长稳或 100 次生命周期验收；这里明确报告实际执行的 20 次。
