# 支持范围与验证证据

ewrtc 0.2.0 是实验性 SDK。下表区分实现能力、历史验证和待验证范围；仓库存在测试代码不等于所有部署环境均已通过测试。

| 范围 | 当前状态 |
| --- | --- |
| Linux x86_64 + native/libjuice × OpenSSL/Mbed TLS | 有历史四组合构建、单元测试和 Chrome 本机互通记录 |
| Luckfox Pico Plus / RV1103 / SC3336 | 有摄像头视频示例及板端运行记录；不代表其他板卡认证 |
| Chrome H.264 / Opus | 本机直连、TURN/UDP、双向媒体与主动 offer 有验证记录 |
| 公网 NAT、复杂防火墙、多网卡 | 尚无完整验证矩阵 |
| Firefox / Safari / 原生 App | 尚无完整互通验证声明 |
| 自定义 PAL、其他 OS | 提供适配接口及 custom PAL 测试；完整移植由接入者验证 |
| IPv6、DataChannel、TURN/TCP、TURN/TLS、自动 ICE restart、动态码率 | 当前不支持 |

## 自动化检查

`.github/workflows/ci.yml` 定义 Linux GCC / Clang 构建、CTest、架构与文档检查、安装消费者验证和最小示例。工作流在 GitHub 的实际运行结果以 Actions 页面为准。

浏览器、板卡和网络命名空间测试需要额外环境，不作为基础 CI 已覆盖能力。

本次发布整理的已执行检查和未验证项见 [发布整理验证](release-validation.md)。

## 历史记录

- [最初 x86_64 验证](validation.md)
- [模块重构验证](modular-validation.md)
- [context 验证](context-validation.md)
- [接收与主动 offer 验证](validation-data/receive-offer/validation.md)
- [Luckfox 接入与局限](../examples/luckfox/README.md)

这些记录形成于 2026-09-25 至 2026-09-26，当时尚无可关联的仓库提交号，不能据此认定当前修订已重跑所有场景。较早报告里的“ARM 尚未验证”等描述仅适用于该报告日期。后续记录应附提交 SHA、构建选项、依赖版本、运行命令及原始结果。
