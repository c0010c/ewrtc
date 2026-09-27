# ewrtc：协议组件、PAL 与平台实现

设备端采用纯 C。协议组件拥有协议状态机，通过按实例注入的 PAL 使用平台服务；Session 组合组件，context 管理共享 worker、准备与清理线程。当前提供 Linux PAL，Google WebRTC 源码只用于参考。

## 模块与依赖

下表为源码内部 target，不是安装包公开接口。应用只链接 `ewrtc::ewrtc` 或 `ewrtc::session`，另按需链接 `ewrtc::pal_linux`。协议模块的接口头位于 `src/<模块>/<模块>.h`，完整布局继续保留在模块自己的 `private.h` 或 `.c` 中。跨模块只引用模块接口，不引用其实现私有头。参见 [公开 API 边界](public-api.md)。

| target | 职责 | 自有组件依赖 |
|---|---|---|
| `ewrtc_common` | 错误码、地址、字节序、ASCII、UTC 转换 | 无 |
| `ewrtc_pal` | 平台服务声明、能力校验、内存和日志辅助 | common |
| `ewrtc_pal_linux` | Linux 平台实现 | pal |
| `ewrtc_crypto` | HMAC-SHA1、MD5；组件内分派 OpenSSL/Mbed TLS 后端 | common |
| `ewrtc_sdp` | Chrome offer 子集解析、answer 生成 | pal |
| `ewrtc_stun` | 报文、完整性、指纹、长期凭据密钥 | pal、crypto |
| `ewrtc_turn` | TURN/UDP 分配、认证、权限、续期、Send/Data Indication | pal、stun、crypto |
| `ewrtc_ice` | 候选、连通性检查、角色与选路、持续授权、数据传输 | pal；native 后端另依赖 stun、turn |
| `ewrtc_dtls` | DTLS 1.2 服务端、指纹验证、SRTP exporter | pal |
| `ewrtc_srtp` | libSRTP 双向媒体保护 | pal |
| `ewrtc_rtp` | RTP/compound RTCP 编解码 | common |
| `ewrtc_media` | H.264/Opus、反馈、发送时间线、RTX 缓存 | pal、rtp |
| `ewrtc_session` | 协商、组件连接、context 调度、队列、回调、统计 | 所需协议组件；不依赖 pal_linux |

协议组件不能引用平台提供者、Session 或其他组件的私有头。公共头不暴露第三方类型。PAL 不管理协议后端；后端适配接口留在 ICE、DTLS、Crypto 各自组件内部。Crypto 的模块分派与两个第三方实现分别编译。

自有网络调用只通过 PAL；无需平台服务的 RTP/RTCP 保持独立。Media 输出明文 RTP/RTCP，由 Session 依次调用 SRTP 和 ICE；接收反向处理。DTLS 通过发送回调连接 ICE，密钥交给 SRTP。SDP 只接收数据，不持有其他协议对象。

Media 内部分成生命周期、公共 RTP 发送、H.264、Opus、RTCP 反馈与缓存实现；Session 内部分成公共生命周期、队列/线程、协商/连接和统计实现。它们仍分别是一个独立内部模块。

## PAL 与显式初始化

`include/ewrtc/pal/` 按 memory、clock、random、threads、network、events、log 分组，`pal.h` 汇总。Linux 声明位于 `ewrtc/platform/linux.h`，实现位于 `src/platform/linux.c`。

```c
#include <ewrtc.h>
#include <ewrtc/platform/linux.h>

ewrtc_context_config runtime;
ewrtc_context_config_init(&runtime);
runtime.pal = *ewrtc_pal_linux();
runtime.pal.clock.ctx = &my_clock;
runtime.pal.clock.monotonic_ms = my_monotonic_ms;
runtime.pal.clock.utc_us = my_utc_us;
ewrtc_session_config config;
ewrtc_session_config_init(&config);
```

组件创建时复制配置值和服务表，不修改全局 PAL。各服务组有独立 `ctx`，替换时不影响其他组；上下文必须存活到组件销毁。配置字符串在创建时复制或同步消费。日志回调同步消费文本，可为空。

能力校验细化到分配/释放、resize、单调时间、UTC、随机数、互斥量、线程、条件变量、UDP 发送、UDP socket、DNS、网卡枚举和事件等待。组件只检查实际使用的能力：

- SDP、SRTP 需要分配/释放；RTP/RTCP 不需要 PAL。
- TURN 需要分配/释放、单调时间、随机数、UDP 发送和同步 DNS；不需要线程、锁、收包、开关 socket、UTC 或 resize。
- native ICE 需要分配/释放、单调时间、随机数和网络；libjuice 适配层需要分配/释放和互斥量。
- DTLS 需要分配/释放、UTC 和单调时间（包括 deadline 查询）。
- Media 需要分配/释放、resize、两种时间和随机数；context 另需线程、同步与事件等待服务。

UDP 非阻塞，暂不可用返回 `EWRTC_AGAIN`；成功发送意味着整份报文被同步接受。Linux 内部重试 EINTR，丢弃截断报文并返回 `EWRTC_INVALID`。地址是主机字节序的 IPv4 和端口；句柄不透明。创建失败清空输出句柄，清理已取得资源。

间隔使用单调毫秒，日期和 RTCP SR 使用 Unix UTC 微秒。条件等待使用相对毫秒，允许虚假唤醒。随机函数必须填满或报错，不回退为时间戳随机数。分配器满足标准 C 对齐，resize 失败保留旧内存；在 Session/libjuice 下必须线程安全。自有对象和缓存走实例分配器；Linux 提供者的 OS 句柄包装及第三方内部内存另行管理。

## TURN 与 ICE 的所有权

native ICE 拥有一个 UDP socket，TURN 借用它。ICE 统一收包，把数据报送给 `ewrtc_turn_receive`；TURN 匹配服务器地址、事务 ID 和方法，处理控制响应或通过 `recv` 回调交回解封装的对端地址与载荷。其他报文继续由 ICE 处理。

内部 TURN 模块接口位于 `src/turn/turn.h`，由 ICE 或内部测试驱动：

- `create/start`：消费服务器名、复制凭据与服务表，启动分配。
- `add_permission/has_permission`：按对端 IP 管理权限；可在分配完成前登记，最多 32 个不同 IP。同 IP 不同端口共用权限，不同 IP 的权限响应相互隔离。
- `receive/tick`：调用方投递报文并驱动；TURN 从 PAL 读取当前时间。重复、不相关报文返回 AGAIN；非法或完整性错误报文丢弃，终止状态通过回调及 `get_status` 查询。
- `send`：要求分配及权限有效；报文不排队。暂不可发返回 AGAIN，由调用方决定丢弃或重试。
- `destroy`：尽力发送零 lifetime Refresh，不等待服务器回复，不关闭借用 socket。

ICE 等待 TURN 分配成功或终止后才报告候选收集完成，避免在重试期间提前结束收集；STUN 地址收集保留 5 秒等待上限。

TURN 内部保留待发送控制报文；UDP 暂不可用后重试。每次事务 8 秒超时，初始重传间隔 500 ms，上限 4 秒；重传保留同一事务 ID。认证重试使用新事务，最多两次 challenge。普通认证响应必须验证完整性；438 可省略完整性字段，提供时仍验证。分配按服务器 lifetime 过期，提前在 60 秒或 lifetime 一半时续期；权限有效期 300 秒，每 60 秒续期。失败停止当前 TURN 对象；重建需重新创建。

TURN 的协议范围仍是 IPv4/UDP、传统长期凭据 HMAC-SHA1/MD5 与 Send/Data Indication。没有新增 ChannelData、TCP/TLS、SHA-256 凭据协商或独立线程。参考 [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656.html) 与 [RFC 8489 §9.2](https://www.rfc-editor.org/rfc/rfc8489.html#section-9.2)。

内部开发示例 `examples/components/turn_example.c` 通过 PAL 创建 socket、收包、驱动 TURN。释放顺序必须是 TURN 在前、socket 在后；示例要求对端为 UDP echo 服务。

## 驱动、错误与生命周期

同一底层对象的调用由调用方串行执行，不在其回调中销毁。回调参数和报文视图借用内存，只在约定期间有效。组件通过 PAL 执行 I/O，协议重传策略和超时状态留在组件内部。

Session 固定归属 context worker，默认每个 context 一个 I/O worker 和一个准备线程。Linux 通过 epoll/eventfd 与协议 deadline 驱动，无固定 20 ms 空闲扫描。输入复制后排队，业务回调在所属 worker 串行执行；回调可提交输入、查询统计和请求 close，不得调用任何 session/context 的同步 destroy。关闭先由 worker 注销并移交对象，再由准备线程执行可能阻塞的后端销毁，最后回到所属 worker 发出 CLOSED；destroy 等待此回调结束及所有引用退出。应用协调 destroy 与其他 API。完整契约见 [context 接入说明](context.md)。

libjuice 后台事件复制到有界队列，由 tick/drain 投递。独立使用时默认上限 256 项、512 KiB；Session 集成时改用 context/单路预算和每路控制槽。普通 RTP/RTX 容量不足丢包并计数；控制交付失败或真实 NOMEM 锁存错误。成功入队和首次交付错误都唤醒 worker，剩余积压主动续调度。Session 的内部关键事件入队失败使用无分配错误锁存，报告原始错误、进入 FAILED、清理组件并报告 CLOSED；该路径不再依赖成功入队。工作线程等待失败也走该路径。

发送链路保留错误码。AGAIN/BACKPRESSURE 作为暂时压力上报，不直接终止媒体会话，不重放已经部分发送的整帧；致命发送错误进入 FAILED。Media 的 tick 返回 RTCP 发送错误。OpenSSL 适配器保留尚未发送的 DTLS 输出，Mbed TLS 将暂不可发映射为 WANT_WRITE。失败会话不继续驱动握手，恢复采用关闭、销毁、重建。

默认限制：发送队列 1 MiB、最多 4096 项；RTX 缓存 2 MiB、2000 ms。H.264 发送、Opus 双向、BUNDLE/RTCP mux、trickle ICE、DTLS 1.2 与 AES128_CM_SHA1_80 范围不变。

## 第三方与构建边界

OpenSSL 内部熵源、内存和 DTLS 超时由库管理；Mbed TLS 内部熵源、DRBG 和内存由库管理，适配器定时器走 PAL。libjuice 的 socket、DNS、线程、内存由它自行管理。libSRTP 内部资源不经过实例分配器，进程初始化使用原子同步。libjuice 全局日志首次使用时禁用；适配器诊断走实例日志。

Linux PAL 不代表这些第三方已经支持其他 OS/RTOS。当前只验证 Linux，不提供旧 API 包装或迁移文档。

`EWRTC_COMPONENTS` 自动补齐组件依赖。`EWRTC_WITH_LINUX_PAL=ON` 在构建 Session 时额外构建 Linux 提供者，Session 与聚合库都不链接或包含它；应用显式链接 `ewrtc::pal_linux`。OFF 时完整 SDK 可以只使用应用自定义 PAL。

仅构建 `sdp;rtp` 不查找密码、TLS、SRTP、libjuice 或线程依赖。TURN 不依赖 ICE、DTLS、SRTP、Linux 或线程。libjuice-only ICE 不依赖自有 TURN。组件和聚合库使用 OBJECT target 共用编译结果，聚合库不重复收录组件。

`tests/check_architecture.py` 检查平台调用、跨组件私有头、公共依赖、第三方适配位置和 Session 反向依赖。`tests/verify_builds.py` 覆盖四后端、所有内部模块的独立构建、公开 SDK 安装消费、C/C++ 头文件独立编译、协议组件拒绝导出，以及关闭 Linux PAL 的构建。实测结果见 [验证记录](modular-validation.md)。
