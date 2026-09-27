# context 与事件驱动改造验证

> 这是对应重构阶段的历史验证记录。此后的公开接口已收窄，独立协议组件不再安装或对外导出；当前规则见 [API 边界说明](public-api.md)。

日期：2026-09-25。范围：显式 context、共享 worker、准备/清理线程、PAL 事件等待、协议 deadline、队列预算与控制预留、完整收包处理配额、两阶段关闭，以及新接口迁移。运行环境是当前 Linux x86_64 开发主机；未进行目标嵌入式板卡验证。

## 通过的检查

| 检查 | 结果 |
|---|---|
| native × OpenSSL/Mbed TLS | 两个构建各 10/10 项 CTest 通过 |
| libjuice × OpenSSL/Mbed TLS | 两个构建各 8/8 项 CTest 通过 |
| 同时启用所有后端的 ASan/UBSan | 10/10 项通过，开启泄漏检查；包括交叉后端协商 |
| TSan | native 和 libjuice 的 context 验收均通过，无数据竞争报告 |
| 独立组件与消费 | 13 个组件构建、安装、外部链接通过；聚合库消费通过 |
| 自定义 PAL | 注入应用 C11 服务，两路共享 context 通过；关闭 Linux PAL 的 SDK 不引用 Linux 提供者或 pthread |
| Chrome 双向互通 | 4 个后端组合 × SDK/浏览器发 offer，共 8/8 通过；实际 H.264 解码和双向音频/视频接收检查通过 |

多路验收覆盖 2/4/8 路，使用独立发送与接收 context；默认每个 context 两个 SDK 线程，四路场景另外验证两个 I/O worker。八路稳定段发送 150 轮、每轮间隔约 33 ms，每路发送 16 KiB 合成 H.264 AU 与 Opus 包。之后有超过单轮 64 包配额的受控突发，以及关闭、替换一路时其他路继续接收的检查。合成 AU 用于 SDK 组帧和传输检查；可解码视频由独立 Chrome 测试验证。

边界验收包括：

- worker 空闲时无限等待、先唤醒后等待、积压无新增输入时继续排空。
- 一路占满全局普通配额后，另一条路仍能交付控制事件并关闭。
- libjuice 媒体超额丢包、释放配额后恢复收流，不因普通媒体拥塞断线。
- 首次入站媒体分配失败时，休眠 worker 被唤醒并报告 NOMEM。
- native 与 libjuice 慢 DNS 期间关闭新连接，已有两路持续接收；destroy 等解析和后台清理完成。
- 远端 candidate DNS 失败报告无效候选，已建立的流继续接收。
- 最终 CLOSED 回调阻塞时 destroy 不提前返回；回调内销毁任意 session/context 返回 STATE。
- 四个应用线程并发创建、销毁共 200 个 session；最终队列、控制预留、session 和 SDK 分配计数归零。
- 部分线程/等待器创建失败、事件注册失败、session 分配失败的资源回滚。
- 业务回调故意留下 OpenSSL 错误队列记录时，后续共享 worker 握手仍正常。

## 本机八路指标

以下队列数值来自接收 context；丢帧取自组帧统计。RSS 是整个测试进程的历史峰值，包含发送端、接收端、第三方库和先前测试场景，不能作为单个 context 或目标设备内存占用。

| 后端 | 普通队列峰值（字节） | 控制预留（字节） | 丢帧 | 调度等待最大值（ms） | 定时器延误最大值（ms） |
|---|---:|---:|---:|---:|---:|
| native / OpenSSL | 265452 | 1146880 | 0 | 2 | 1 |
| native / Mbed TLS | 265452 | 1146880 | 0 | 4 | 1 |
| libjuice / OpenSSL | 340663 | 1146880 | 0 | 3 | 1 |
| libjuice / Mbed TLS | 205469 | 1146880 | 0 | 5 | 1 |

完整 CPU、RSS、回调与单轮耗时见 [多路指标](validation-data/context/stream-metrics.json)。这些是短时本机功能压力测试，不是长期稳定性或嵌入式硬件性能保证。同步 DNS 没有取消保证，destroy 可等待解析返回；第三方线程与内部内存不包含在 SDK 线程/队列预算内。

## 复现与原始记录

在项目根目录执行：

```bash
python3 tests/verify_builds.py --juice-source cmake-build-debug/_deps/libjuice-src

cmake -S . -B build-context-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_LIBJUICE="$PWD/cmake-build-debug/_deps/libjuice-src" \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-context-asan -j8
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-context-asan --output-on-failure

# 将 flags 换为 -fsanitize=thread，分别启用 native 或 libjuice 后：
TSAN_OPTIONS=halt_on_error=1 build-context-tsan/ewrtc_context_tests
TSAN_OPTIONS=halt_on_error=1 build-context-tsan-juice/ewrtc_context_tests

python3 tests/test_browser_duplex.py --demo cmake-build-debug/ewrtc_demo \
  --video /tmp/ewrtc-context-720p.frames --output /tmp/ewrtc-context-browser
```

浏览器素材为 720p30、约 1 Mbps、10 秒的 H.264 Constrained Baseline 测试源，带 AUD 和重复 SPS/PPS，使用 `examples/prepare_media.py` 转成 `.frames`。合成多路 C 测试不需要外部素材或服务器。

原始日志：[构建矩阵](validation-data/context/build-matrix.log)、[ASan/UBSan](validation-data/context/asan-ubsan.log)、[native TSan](validation-data/context/tsan-native.log)、[libjuice TSan](validation-data/context/tsan-juice.log)、[Chrome 结果](validation-data/context/chrome-duplex.json)。各后端完整 CTest 输出保存在同一目录。
