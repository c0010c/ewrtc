# 多路 context 与事件驱动

面向嵌入式 Linux 的 2–8 路连接。context 管运行资源；session 独立持有 ICE、DTLS、SRTP、socket、TURN、媒体和单路限额；业务负责信令运输、鉴权、重连、解码和显示。

## 创建与配置

```c
ewrtc_context_config runtime;
ewrtc_context_config_init(&runtime);
runtime.pal = *ewrtc_pal_linux();
runtime.worker_count = 1;
runtime.max_sessions = 8;
ewrtc_context *context;
ewrtc_result result = ewrtc_context_create(&runtime, &context);
if (result) return result;

ewrtc_session_config config;
ewrtc_session_config_init(&config);
config.video_direction = EWRTC_RECVONLY;
config.audio_direction = EWRTC_RECVONLY;
ewrtc_session *session;
result = ewrtc_session_create(context, &config, &callbacks, user, &session);
if (result) {
    ewrtc_context_destroy(context);
    return result;
}
/* 创建更多 session 时复用 context；应用按自己的信令流程提交 offer/answer。 */
```

旧 `ewrtc_config` 和四参数 session 创建入口已移除。所有调用方需要重新编译并迁移；不提供隐式默认 context。PAL 只存在于 context 配置，各服务表复制，服务的 ctx 仍由应用持有，必须覆盖 context 的完整生命周期。

零值配置使用默认值：一个 I/O worker、最多八个 session、8 MiB 队列预算、每路 64 个控制槽。每个 context 另有一个准备线程；libjuice 自身线程不包含在这两个 SDK 线程中。session 绑定登记数量最少的 worker，平局按序号选择，直到 destroy 不迁移。worker 数不能超过 session 上限。

首次 offer 操作异步触发证书与传输对象创建。应用传入的 SDP/candidate 域名在准备线程解析；数字 IPv4 直接解析，`.local` 保留现有 peer-reflexive 处理方式。libjuice 自身服务器解析线程由其后端管理，销毁时同样不能阻塞 I/O worker。

## 事件和预算

PAL 的 events 组提供等待器创建/销毁、socket 注册/注销、等待和跨线程唤醒；socket 与等待器必须使用匹配的平台提供者。注册标识是非零代次，不是对象指针。Linux 使用水平触发 epoll 和非阻塞 eventfd，唤醒先于 wait 仍然有效。events wait 和 condition wait 的 `UINT32_MAX` 均代表无限等待。

ICE、TURN、DTLS、媒体的 `next_deadline` 返回绝对单调毫秒，`UINT64_MAX` 代表无定时工作。原有独立组件 tick 继续可用。worker 按最早 deadline 等待，不再每 20 ms 扫描所有连接；协议暂时无法发送且保留待发数据时仍可安排 20 ms 重试。

每次轮转最多消费 32 条命令/控制事件和 64 个入站包。native 包在 worker 内直接推进协议和媒体处理；libjuice 保留跨线程有界队列，消费后直接处理包。不会将读取的 64 个包重新放进只消费 32 条的 session 命令队列。剩余积压重新调度，不依赖下一次网络事件。数量配额不是耗时上限：单个回调或发送 AU 仍可能耗时较长，业务回调必须快速交接数据。

单路普通队列默认上限 1 MiB，包含节点头和正在处理但尚未释放的任务。SDP 入队为域名改写保留最多 64 KiB 的容量；对应容量计入预算。context 总队列预算同时涵盖每路预分配控制槽、普通队列及正在处理的队列任务。预留在 session 创建时兑现，容量不足则创建返回 BACKPRESSURE，不侵占已有 session 的预留。

每个控制槽可装载 2048 字节的控制包及内部事件头。后端状态、local candidate、gathering done、DTLS/STUN/TURN/RTCP 控制数据不可因其他流占用普通预算而丢弃。普通媒体不能使用控制槽。应用提交的协商命令在接受前检查配额，接受后保持顺序，不因媒体拥塞驱逐。

发送超额返回 BACKPRESSURE；libjuice 接收 RTP/RTX 普通队列超额则丢弃新包、计数，由既有 NACK/PLI/组帧超时机制恢复。控制槽耗尽则报告该路交付错误并关闭。close、准备完成、清理完成、首个关键错误使用固定状态，普通队列满不妨碍交付。真实 NOMEM 会报告错误，不承诺系统内存耗尽时的绝对跨路隔离。

队列预算不是 SDK 总内存上限：协议对象、组帧、RTX、线程栈、socket 缓冲和第三方内部内存另计。PAL 可自行定制线程创建与栈大小。

## 关闭与回调

同一 session 的全部业务回调在固定 worker 串行执行。回调数据借用至回调返回；异步解码或显示须复制到业务有界队列。同一 context 的多个 session 可能共享线程，不允许在业务回调内阻塞等待。

`close()` 异步、幂等、不分配队列内存，可在回调中调用；之后拒绝新输入。两阶段关闭：

1. 所属 worker 在当前调用退出后停止驱动，取消事件与定时登记，停止向业务投递后端事件，将协议对象独占移交。
2. 准备线程执行可能阻塞的销毁，包括 libjuice resolver/producer join；排队中的清理优先于新的准备任务。已执行的同步 DNS 不可抢占。

清理完成通知使用固定状态，由所属 worker 发出最后一次 CLOSED。业务回调返回、对象和生产者及唤醒引用退出后，session destroy 才返回。准备/清理线程不调用业务回调。慢 DNS 可能延迟后续准备与清理，因此 destroy 没有固定完成时限；已有连接继续由 I/O worker 驱动。

所有 SDK 业务回调内调用任意 session/context 的 destroy 均返回 STATE。应用必须排除裸句柄与 destroy 的并发访问。仍有未 destroy 的 session 时，context destroy 返回 STATE 并保持有效。调用顺序：关闭各 session、销毁各 session、最后销毁 context。

## 观测与验证

`ewrtc_context_get_stats` 返回 session/worker/SDK 线程数量、控制预留、普通队列当前和峰值、背压、接收拥塞丢包，以及调度等待、定时器延误、回调和单轮处理最大耗时（毫秒）。session 统计保留原有接口。

`ewrtc_context_tests` 覆盖 2/4/8 路合成 H.264 AU/Opus 传输、动态替换、多个 worker、全局预算耗尽、慢 DNS 两阶段关闭和 libjuice 分配失败唤醒。DNS 故障使用进程内测试域名拦截，数字地址探测不被阻塞，不依赖外部服务器。输出 CPU、RSS 峰值、队列和调度指标；这些是测试主机数据，不是目标板性能承诺。

`tests/verify_builds.py` 验证后端矩阵、内部模块独立构建和公开 SDK 安装消费、自定义 PAL。自定义 PAL 测试使用应用注入的 C11 服务，不链接 SDK Linux PAL；SDK 的纯 native 聚合库仍不直接引用 pthread。

本次实测覆盖、短时指标与原始日志见 [context 验证记录](context-validation.md)。
